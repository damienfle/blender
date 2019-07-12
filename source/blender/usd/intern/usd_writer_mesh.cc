#include "usd_writer_mesh.h"
#include "usd_hierarchy_iterator.h"

#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

extern "C" {
#include "BKE_anim.h"
#include "BKE_library.h"
#include "BKE_material.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
}

USDGenericMeshWriter::USDGenericMeshWriter(const USDExporterContext &ctx) : USDAbstractWriter(ctx)
{
}

void USDGenericMeshWriter::do_write(HierarchyContext &context)
{
  Object *object_eval = context.object;
  bool needsfree = false;
  Mesh *mesh = get_export_mesh(object_eval, needsfree);

  if (mesh == NULL) {
    printf("USD-\033[31mSKIPPING\033[0m object %s  type=%d mesh = NULL\n",
           object_eval->id.name,
           object_eval->type);
    return;
  }

  try {
    write_mesh(context, mesh);

    if (needsfree) {
      free_export_mesh(mesh);
    }
  }
  catch (...) {
    if (needsfree) {
      free_export_mesh(mesh);
    }
    throw;
  }
}

void USDGenericMeshWriter::free_export_mesh(Mesh *mesh)
{
  BKE_id_free(NULL, mesh);
}

struct USDMeshData {
  pxr::VtArray<pxr::GfVec3f> points;
  pxr::VtIntArray face_vertex_counts;
  pxr::VtIntArray face_indices;
  std::map<short, pxr::VtIntArray> face_groups;

  /* The length of this array specifies the number of creases on the surface. Each element gives
   * the number of (must be adjacent) vertices in each crease, whose indices are linearly laid out
   * in the 'creaseIndices' attribute. Since each crease must be at least one edge long, each
   * element of this array should be greater than one. */
  pxr::VtIntArray crease_lengths;
  /* The indices of all vertices forming creased edges. The size of this array must be equal to the
   * sum of all elements of the 'creaseLengths' attribute. */
  pxr::VtIntArray crease_vertex_indices;
  /* The per-crease or per-edge sharpness for all creases (Usd.Mesh.SHARPNESS_INFINITE for a
   * perfectly sharp crease). Since 'creaseLengths' encodes the number of vertices in each crease,
   * the number of elements in this array will be either len(creaseLengths) or the sum over all X
   * of (creaseLengths[X] - 1). Note that while the RI spec allows each crease to have either a
   * single sharpness or a value per-edge, USD will encode either a single sharpness per crease on
   * a mesh, or sharpnesses for all edges making up the creases on a mesh. */
  pxr::VtFloatArray crease_sharpnesses;
};

void USDGenericMeshWriter::write_mesh(HierarchyContext &context, Mesh *mesh)
{
  pxr::UsdTimeCode timecode = get_export_time_code();
  // printf("USD-\033[32mexporting\033[0m mesh  %s → %s  mesh = %p\n",
  //        mesh->id.name,
  //        usd_path_.GetString().c_str(),
  //        mesh);

  pxr::UsdGeomMesh usd_mesh = pxr::UsdGeomMesh::Define(stage, usd_path_);

  USDMeshData usd_mesh_data;
  get_geometry_data(mesh, usd_mesh_data);

  usd_mesh.CreatePointsAttr().Set(usd_mesh_data.points, timecode);
  usd_mesh.CreateFaceVertexCountsAttr().Set(usd_mesh_data.face_vertex_counts, timecode);
  usd_mesh.CreateFaceVertexIndicesAttr().Set(usd_mesh_data.face_indices, timecode);

  if (!usd_mesh_data.crease_lengths.empty()) {
    usd_mesh.CreateCreaseLengthsAttr().Set(usd_mesh_data.crease_lengths, timecode);
    usd_mesh.CreateCreaseIndicesAttr().Set(usd_mesh_data.crease_vertex_indices, timecode);
    usd_mesh.CreateCreaseSharpnessesAttr().Set(usd_mesh_data.crease_sharpnesses, timecode);
  }

  // TODO(Sybren): figure out what happens when the face groups change.
  if (frame_has_been_written_) {
    return;
  }

  assign_materials(context, usd_mesh, usd_mesh_data.face_groups);
}

static void get_vertices(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  usd_mesh_data.points.reserve(mesh->totvert);

  const MVert *verts = mesh->mvert;
  for (int i = 0; i < mesh->totvert; ++i) {
    usd_mesh_data.points.push_back(pxr::GfVec3f(verts[i].co));
  }
}

static void get_loops_polys(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  /* Only construct face groups (a.k.a. geometry subsets) when we need them for material
   * assignments. */
  bool construct_face_groups = mesh->totcol > 1;

  usd_mesh_data.face_vertex_counts.reserve(mesh->totpoly);
  usd_mesh_data.face_indices.reserve(mesh->totloop);

  MLoop *mloop = mesh->mloop;
  MPoly *mpoly = mesh->mpoly;
  for (int i = 0; i < mesh->totpoly; ++i, ++mpoly) {
    MLoop *loop = mloop + mpoly->loopstart;
    usd_mesh_data.face_vertex_counts.push_back(mpoly->totloop);
    for (int j = 0; j < mpoly->totloop; ++j, ++loop) {
      usd_mesh_data.face_indices.push_back(loop->v);
    }

    if (construct_face_groups) {
      usd_mesh_data.face_groups[mpoly->mat_nr].push_back(i);
    }
  }
}

static void get_creases(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  const float factor = 1.0f / 255.0f;

  MEdge *edge = mesh->medge;
  float sharpness;
  for (int edge_idx = 0, totedge = mesh->totedge; edge_idx < totedge; ++edge_idx, ++edge) {
    if (edge->crease == 0) {
      continue;
    }

    if (edge->crease == 255) {
      sharpness = pxr::UsdGeomMesh::SHARPNESS_INFINITE;
    }
    else {
      sharpness = static_cast<float>(edge->crease) * factor;
    }

    usd_mesh_data.crease_vertex_indices.push_back(edge->v1);
    usd_mesh_data.crease_vertex_indices.push_back(edge->v2);
    usd_mesh_data.crease_lengths.push_back(2);
    usd_mesh_data.crease_sharpnesses.push_back(sharpness);
  }
}

void USDGenericMeshWriter::get_geometry_data(const Mesh *mesh, struct USDMeshData &usd_mesh_data)
{
  get_vertices(mesh, usd_mesh_data);
  get_loops_polys(mesh, usd_mesh_data);
  get_creases(mesh, usd_mesh_data);
}

void USDGenericMeshWriter::assign_materials(
    const HierarchyContext &context,
    pxr::UsdGeomMesh usd_mesh,
    const std::map<short, pxr::VtIntArray> &usd_face_groups)
{
  if (context.object->totcol == 0) {
    return;
  }

  /* Binding a material to a geometry subset isn't supported by the Hydra GL viewport yet,
   * which is why we always bind the first material to the entire mesh. See
   * https://github.com/PixarAnimationStudios/USD/issues/542 for more info. */
  bool mesh_material_bound = false;
  for (short mat_num = 0; mat_num < context.object->totcol; mat_num++) {
    Material *material = give_current_material(context.object, mat_num + 1);
    if (material == nullptr) {
      continue;
    }

    pxr::UsdShadeMaterial usd_material = ensure_usd_material(material);
    usd_material.Bind(usd_mesh.GetPrim());

    /* USD seems to support neither per-material nor per-face-group double-sidedness, so we just
     * use the flag from the first non-empty material slot. */
    usd_mesh.CreateDoubleSidedAttr(
        pxr::VtValue((material->blend_flag & MA_BL_CULL_BACKFACE) == 0));

    mesh_material_bound = true;
    break;
  }

  if (!mesh_material_bound) {
    /* Blender defaults to double-sided, but USD to single-sided. */
    usd_mesh.CreateDoubleSidedAttr(pxr::VtValue(true));
  }

  if (!mesh_material_bound || usd_face_groups.size() < 2) {
    /* Either all material slots were empty or there is only one material in use. As geometry
     * subsets are only written when actually used to assign a material, and the mesh already has
     * the material assigned, there is no need to continue. */
    return;
  }

  // Define a geometry subset per material.
  for (auto face_group_iter : usd_face_groups) {
    short material_number = face_group_iter.first;
    const pxr::VtIntArray &face_indices = face_group_iter.second;

    Material *material = give_current_material(context.object, material_number + 1);
    if (material == nullptr) {
      continue;
    }

    pxr::UsdShadeMaterial usd_material = ensure_usd_material(material);
    pxr::TfToken material_name = usd_material.GetPath().GetNameToken();

    pxr::UsdShadeMaterialBindingAPI api = pxr::UsdShadeMaterialBindingAPI(usd_mesh);
    pxr::UsdGeomSubset usd_face_subset = api.CreateMaterialBindSubset(material_name, face_indices);
    usd_material.Bind(usd_face_subset.GetPrim());
  }
}

USDMeshWriter::USDMeshWriter(const USDExporterContext &ctx) : USDGenericMeshWriter(ctx)
{
}

Mesh *USDMeshWriter::get_export_mesh(Object *object_eval, bool & /*r_needsfree*/)
{
  return object_eval->runtime.mesh_eval;
}
