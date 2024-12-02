#include "NOD_register.hh"
#include "node_shader_util.hh"

namespace blender::nodes::node_shader_equation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("A").default_value(1.0f).description("Term A");
  b.add_output<decl::Float>("Result");
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  NodeItem inputA = get_input_value("A", NodeItem::Type::Float);
  return inputA + inputA;
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_equation_cc

void register_node_type_sh_equation()
{
  printf("Registering Node Shader Equation\n");

  namespace file_ns = blender::nodes::node_shader_equation_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, SH_NODE_EQUATION, "Equation", NODE_CLASS_CONVERTER);

  ntype.declare = file_ns::node_declare;
  // ntype.gpu_fn = file_ns::node_shader_gpu_gamma;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(&ntype);
}
// NOD_REGISTER_NODE(node_register)
