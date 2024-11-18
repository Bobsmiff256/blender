// #include <numeric>

// #include "BLI_math_geom.h"
// #include "BLI_math_matrix.h"
// #include "BLI_math_rotation.h"

// #include "BKE_curves.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "BKE_geometry_set.hh"
#include "BKE_material.h"
#include "DNA_meshdata_types.h";
#include "GEO_mesh_primitive_grid.hh"
#include "bke_mesh.hh"
#include "node_geometry_util.hh"
#include <intern/rna_internal_types.hh>

namespace blender::nodes::node_geo_pizza_cc {

NODE_STORAGE_FUNCS(NodeGeometryPizza)

// Forward declarations
static Mesh *create_pizza_mesh(int olive_count,
                               float radius,
                               float olive_radius,
                               IndexRange &base_polys,
                               IndexRange &olive_polys);
static blender::float2 get_olive_center(const int olive_index,
                                        const int num_olives,
                                        const float placement_radius);
static const float get_olive_placement_radius(const float radius);

// Defines the inputs and outputs of the node
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Radius")
      .default_value(1.0f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description("Size of the pizza");

  b.add_output<decl::Geometry>("Mesh");
  b.add_output<decl::Bool>("Base").field_on_all();
  b.add_output<decl::Bool>("Olives").field_on_all();
  b.add_output<decl::Vector>("UV Map").field_on_all();
}

// Draws the node
static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "olive_count", UI_ITEM_R_EXPAND, "", ICON_NONE);
}

// Called when a node of this type is created
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryPizza *data = MEM_cnew<NodeGeometryPizza>(__func__);
  data->olive_count = 5;
  node->storage = data;
}

// Called on update of properties
static void node_update(bNodeTree *nTree, bNode *node)
{
  const NodeGeometryPizza &storage = node_storage(*node);
  bNodeSocket *out_geometry = (bNodeSocket *)node->outputs.first;
  // bNodeSocket *out_socket_base = out_geometry->next;
  // bNodeSocket *out_socket_olives = out_socket_base->next;

  // Dummy feature for example. When there are too many oilves
  // no longer output the fields
  //  nodeSetSocketAvailability(nTree, out_socket_base, storage.olive_count < 25);
  //  nodeSetSocketAvailability(nTree, out_socket_olives, storage.olive_count < 25);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryPizza &storage = node_storage(params.node());
  const int olive_count = storage.olive_count;
  const float radius = params.extract_input<float>("Radius");
  const float olive_radius = radius / 10;

  IndexRange base_polys, olive_polys;
  Mesh *mesh = create_pizza_mesh(olive_count, radius, olive_radius, base_polys, olive_polys);

  // Build a geometry set to wrap the mesh in, and set it as output
  params.set_output("Mesh", GeometrySet::from_mesh(mesh));

  // Output Base field if necessary
  std::optional<std::string> base_id = params.get_output_anonymous_attribute_id_if_needed("Base");
  if (base_id.has_value()) {
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_span<bool>(
        *base_id, bke::AttrDomain::Face);
    // Selection
    for (const int i : base_polys)
      selection.span[i] = true;
    selection.finish();
  }
  std::optional<std::string> olives_id = params.get_output_anonymous_attribute_id_if_needed(
      "Olives");
  if (olives_id.has_value()) {
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_span<bool>(
        *olives_id, bke::AttrDomain::Face);
    // Selection
    for (const int i : olive_polys)
      selection.span[i] = true;
    selection.finish();
  }

  // output UVs
  std::optional<std::string> uv_id = params.get_output_anonymous_attribute_id_if_needed("UV Map");
  if (uv_id.has_value()) {
    bke::MutableAttributeAccessor uvmap = mesh->attributes_for_write();
    bke::SpanAttributeWriter<blender::float2> uvWriter =
        uvmap.lookup_or_add_for_write_span<blender::float2>(*uv_id, bke::AttrDomain::Corner);

    // Foreach base poly
    for (const int bp : base_polys) {
      int face_corner_offset = mesh->face_offsets()[bp];
      int next_face_offset = mesh->face_offsets()[bp + 1];  // works because the array ends with
                                                            // the total number of corners
      int num_corners = next_face_offset - face_corner_offset;
      // Foreach corner in base
      for (int c = 0; c < num_corners; c++) {
        int vert_idx = mesh->corner_verts()[face_corner_offset + c];
        blender::float3 vert_pos = mesh->vert_positions()[vert_idx];
        // Set uv
        float u = (vert_pos.x + radius) / (2 * radius);
        float v = (vert_pos.y + radius) / (2 * radius);
        float2 uv(u, v);
        uvWriter.span[face_corner_offset + c] = uv;
      }
    }

    const float placement_radius = get_olive_placement_radius(radius);

    // For each olive poly
    for (int olive_index = 0; olive_index < olive_count; olive_index++) {
      const int olive_poly = olive_polys[olive_index];
      const int face_corner_offset = mesh->face_offsets()[olive_poly];
      const int next_face_offset =
          mesh->face_offsets()[olive_poly + 1];  // works because the array ends with the total
                                                 // number of corners
      const int num_corners = next_face_offset - face_corner_offset;
      // Foreach corner in olive
      for (int c = 0; c < num_corners; c++) {
        const int vert_idx = mesh->corner_verts()[face_corner_offset + c];
        blender::float3 vert_pos = mesh->vert_positions()[vert_idx];
        const blender::float2 olive_center = get_olive_center(
            olive_index, olive_count, placement_radius);
        vert_pos.x -= olive_center.x;
        vert_pos.y -= olive_center.y;

        // Set uv
        const float u = (vert_pos.x + olive_radius) / (2 * olive_radius);
        const float v = (vert_pos.y + olive_radius) / (2 * olive_radius);
        const float2 uv(u, v);
        uvWriter.span[face_corner_offset + c] = uv;
      }
    }
  }
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  // Add an editable field for olive count
  const bNode &bnode = builder.node();
  NodeGeometryPizza *node_storage = static_cast<NodeGeometryPizza *>(bnode.storage);
  builder.construct_and_set_matching_fn<mf::CustomMF_Constant<int>>(node_storage->olive_count);
}

static void node_rna(StructRNA *srna)
{
  PropertyRNA *prop;

  // Define a property field
  prop = RNA_def_property(srna, "olive_count", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 127);
  RNA_def_property_ui_text(prop, "Olive Count", "Number of olives on top");

  // Define the getter and setter for this field
  auto getter = [](PointerRNA *ptr, PropertyRNA *prop) {
    const bNode &node = *static_cast<const bNode *>(ptr->data);
    return (int)(node_storage(node).olive_count);
  };

  auto setter = [](PointerRNA *ptr, PropertyRNA *prop, int value) {
    bNode &node = *static_cast<bNode *>(ptr->data);
    node_storage(node).olive_count = value;
  };

  RNA_def_property_int_funcs_runtime(prop, getter, setter, nullptr /*range*/);

  // Define how it updates
  RNA_def_property_update_runtime(prop, rna_Node_socket_update);
  RNA_def_property_update_notifier(prop, NC_NODE | NA_EDITED);

  /*
  RNA_def_node_enum(srna,
                    "color_id",
                    "Color",
                    "",
                    rna_enum_geometry_nodes_gizmo_color_items,
                    NOD_storage_enum_accessors(color_id));
  RNA_def_node_enum(srna,
                    "draw_style",
                    "Draw Style",
                    "",
                    rna_enum_geometry_nodes_linear_gizmo_draw_style_items,
                    NOD_storage_enum_accessors(draw_style)); */
}

static Mesh *create_pizza_mesh(const int olive_count,
                               const float radius,
                               const float olive_radius,
                               IndexRange &base_polys,
                               IndexRange &olive_polys)
{
  constexpr int NUM_SEGMENTS = 32;
  constexpr int OLIVE_SEGMENTS = 8;

  // Compute element counts
  int vert_count = NUM_SEGMENTS + olive_count * OLIVE_SEGMENTS;
  int edge_count = NUM_SEGMENTS + olive_count * OLIVE_SEGMENTS;
  int corner_count = NUM_SEGMENTS + olive_count * OLIVE_SEGMENTS;
  int face_count = 1 + olive_count;

  // Allocate a blank mesh with the correct storage
  Mesh *mesh = BKE_mesh_new_nomain(vert_count, edge_count, face_count, corner_count);

  // Get buffers
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  MutableSpan<int2> edges = mesh->edges_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
  MutableSpan<int> faces = mesh->face_offsets_for_write();

  bke::mesh_smooth_set(*mesh, true);

  // For each face, set the offset of its set of corners
  // in the corner vert and edge arrays
  faces[0] = 0;
  for (const int i : IndexRange(olive_count))
    faces[i + 1] = NUM_SEGMENTS + OLIVE_SEGMENTS * i;

  // First create the base
  // Set vert positions
  const float angle_delta = (M_PI * 2) / NUM_SEGMENTS;
  IndexRange vertRange = IndexRange(NUM_SEGMENTS);
  for (const int v : vertRange) {
    const float angle = v * angle_delta;
    positions[v] = float3(std::cos(angle) * radius, std::sin(angle) * radius, 0);
  }

  // Set edge data
  for (const int e : IndexRange(NUM_SEGMENTS)) {
    int finalVert = e < NUM_SEGMENTS - 1 ? e + 1 : 0;  // Final edge wraps to first vert
    edges[e] = int2(e, finalVert);
  }

  // Set corner data
  for (const int c : IndexRange(NUM_SEGMENTS)) {
    corner_verts[c] = c;
    corner_edges[c] = c;
  }

  // Add olives
  const float olive_rad = olive_radius;
  olive_polys = IndexRange(1, olive_count);
  for (int i = 0; i < olive_count; i++) {
    const int offset = NUM_SEGMENTS + i * OLIVE_SEGMENTS;

    // Olive position
    const float olive_placement_radius = get_olive_placement_radius(radius);
    const blender::float2 olive_center = get_olive_center(i, olive_count, olive_placement_radius);
    const float cx = olive_center.x, cy = olive_center.y;

    // Verts
    const float olive_angle_delta = (M_PI * 2) / OLIVE_SEGMENTS;
    IndexRange oliveVerts = IndexRange(OLIVE_SEGMENTS);
    for (const int v : oliveVerts) {
      const float angle = v * olive_angle_delta;
      positions[offset + v] = float3(
          std::cos(angle) * olive_rad + cx, std::sin(angle) * olive_rad + cy, 0.1f);
    }

    // Edges
    for (const int e : IndexRange(0, OLIVE_SEGMENTS)) {
      const int finalVert = (e + 1) % OLIVE_SEGMENTS;
      edges[e + offset] = int2(e + offset, finalVert + offset);
    }

    // Corners
    for (const int c : IndexRange(OLIVE_SEGMENTS)) {
      corner_verts[c + offset] = c + offset;
      corner_edges[c + offset] = c + offset;
    }
  }

  mesh->tag_loose_verts_none();
  mesh->tag_loose_verts_none();
  mesh->tag_overlapping_none();
  //  mesh->bounds_set_eager(calculate_bounds_cylinder(config));

  std::optional<std::string> uv_map_id; /*(std::nullopt_t); */
  BKE_id_material_eval_ensure_default_slot(reinterpret_cast<ID *>(mesh));

  // Set ranges
  base_polys = IndexRange(0, 1);
  olive_polys = IndexRange(1, olive_count);

  BLI_assert(BKE_mesh_is_valid(mesh));
  return mesh;
}

static blender::float2 get_olive_center(const int olive_index,
                                        const int num_olives,
                                        const float placement_radius)
{
  // Olive 0 is at the center
  float cx = 0, cy = 0;
  if (olive_index > 0) {
    float delta = (M_PI * 2) / (num_olives - 1);
    float angle = delta * (olive_index - 1);
    cx = std::cos((olive_index - 1) * delta) * placement_radius;
    cy = std::sin((olive_index - 1) * delta) * placement_radius;
  }

  return blender::float2(cx, cy);
}

static const float get_olive_placement_radius(const float radius)
{
  return radius / 2;
}

static void node_register()
{
  // printf("pizza node registered!\n");
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, GEO_NODE_PIZZA, "Pizza", NODE_CLASS_GEOMETRY);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  blender::bke::node_type_storage(
      &ntype, "NodeGeometryPizza", node_free_standard_storage, node_copy_standard_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  // ntype.draw_buttons_ex = node_layout_ex;   // Draw buttons on side bar
  ntype.updatefunc = node_update;
  ntype.build_multi_function = node_build_multi_function;

  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_pizza_cc
