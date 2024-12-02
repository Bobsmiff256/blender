/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_rna_define.hh"

#include "node_function_util.hh"
#include <stdint.h>

namespace blender::nodes::node_fn_equation_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::String>("Equation");
  b.add_input<decl::Float>("X").default_value(1.0f);
  b.add_input<decl::Float>("Y").default_value(1.0f);
  b.add_input<decl::Float>("Z").default_value(1.0f);
  b.add_output<decl::Float>("Result");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeFunctionEquation *data = MEM_cnew<NodeFunctionEquation>(__func__);
  data->byte_code = nullptr;  // No bytecode as yet
  node->storage = data;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  // uiItemR(layout, ptr, "axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
  // uiLayoutSetPropSep(layout, true);
  // uiLayoutSetPropDecorate(layout, false);
  // uiItemR(layout, ptr, "pivot_axis", UI_ITEM_NONE, IFACE_("Pivot"), ICON_NONE);
}

class EquationEvaluateFunction : public mf::MultiFunction {
  const int32_t *byte_code_;
  int32_t stack_size_;
  char *socket_name_buffer_ = nullptr;

 public:
  EquationEvaluateFunction(const bNode &node)
  {
    const NodeFunctionEquation *node_storage = static_cast<NodeFunctionEquation *>(node.storage);
    byte_code_ = node_storage->byte_code;
    stack_size_ = node_storage->stack_size;

    // static const mf::Signature signature = []() {
    //   mf::Signature signature;
    //   mf::SignatureBuilder builder{"Equation", signature};
    //   builder.single_input<std::string>("Equation");
    //   builder.single_input<float>("X");
    //   builder.single_input<float>("Y");
    //   builder.single_input<float>("Z");
    //   builder.single_output<float>("Result");
    //   return signature;
    // }();

    static const mf::Signature signature = CreateSignature(node);
    this->set_signature(&signature);
  }

  ~EquationEvaluateFunction()
  {
    if (socket_name_buffer_)
      MEM_freeN(socket_name_buffer_);
  }

  // Copy constructor
  EquationEvaluateFunction(const EquationEvaluateFunction &source)
  {
    printf("EquationEvaluateFunction copy constructor called");

    byte_code_ = source.byte_code_;
    stack_size_ = source.stack_size_;
    socket_name_buffer_ = source.socket_name_buffer_;  // WRONG
  }

  // Move constructor
  EquationEvaluateFunction(EquationEvaluateFunction &&other)
  {
    printf("EquationEvaluateFunction move constuctor called");

    byte_code_ = other.byte_code_;
    stack_size_ = other.stack_size_;
    socket_name_buffer_ = other.socket_name_buffer_;  // WRONG
    other.socket_name_buffer_ = nullptr;              // stop the destructor freeing this
  }

  // Copy assignment operator
  EquationEvaluateFunction &operator=(const EquationEvaluateFunction &other)
  {
    printf("EquationEvaluateFunction copy assignment operator called");

    if (this != &other) {
      byte_code_ = other.byte_code_;
      stack_size_ = other.stack_size_;
      socket_name_buffer_ = other.socket_name_buffer_;  // WRONG
    }

    return *this;
  }

  // Move assignment operator
  EquationEvaluateFunction &operator=(EquationEvaluateFunction &&other)
  {
    printf("EquationEvaluateFunction move assignment operator called");

    if (this != &other) {
      // Free out own buffer
      byte_code_ = other.byte_code_;
      stack_size_ = other.stack_size_;
      socket_name_buffer_ = other.socket_name_buffer_;  // WRONG
      other.socket_name_buffer_ = nullptr;              // stop the destructor freeing this
    }

    return *this;
  }

  mf::Signature CreateSignature(const bNode &node)
  {
    mf::Signature signature;
    mf::SignatureBuilder builder{"Equation", signature};
    // builder.single_input<std::string>("Equation");
    // builder.single_input<float>("X");
    // builder.single_input<float>("Y");
    // builder.single_input<float>("Z");
    // builder.single_output<float>("Result");

    // The socket names in the node don't seem to be permanent
    // so find their total size, then allocate a buffer and make copies
    int total_name_length = 0;
    for (auto in_sock : node.input_sockets()) {
      total_name_length += strlen(in_sock->identifier) + 1;
    }
    socket_name_buffer_ = MEM_cnew_array<char>(total_name_length, __func__);
    char *next_name_buffer = socket_name_buffer_;

    for (auto in_sock : node.input_sockets()) {
      // Copy the name to the name buffer
      size_t name_len = strlen(in_sock->identifier) + 1;
      memcpy(next_name_buffer, in_sock->identifier, name_len);
      builder.single_input(next_name_buffer, *in_sock->typeinfo->base_cpp_type);
      next_name_buffer += name_len;
    }
    builder.single_output<float>("Result");

    return signature;
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const final
  {
    const VArray<float> x_values = params.readonly_single_input<float>(1, "X");
    const VArray<float> y_values = params.readonly_single_input<float>(2, "Y");
    const VArray<float> z_values = params.readonly_single_input<float>(3, "Z");

    MutableSpan results = params.uninitialized_single_output<float>(4, "Result");

    mask.foreach_index(
        [&](const int64_t i) { results[i] = x_values[i] + y_values[i] + z_values[i]; });
  }

  ExecutionHints get_execution_hints() const final
  {
    ExecutionHints hints;
    hints.min_grain_size = 512;
    return hints;
  }
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  // static auto exec_preset = mf::build::exec_presets::AllSpanOrSingle();
  // static auto dummy = mf::build::SI4_SO<std::string, float, float, float, float>(
  //     "Eval", [](std::string s, float x, float y, float z) { return x + y + z; }, exec_preset);
  // builder.set_matching_fn(dummy);

  const bNode &node = builder.node();
  /* builder.construct_and_set_matching_fn<AlignRotationToVectorFunction>(
      math::Axis::from_int(node.custom1), NodeAlignEulerToVectorPivotAxis(node.custom2));
  */
  // builder.construct_and_set_matching_fn<EquationEvaluateFunction>(
  //     static_cast<NodeFunctionEquation *>(node.storage)->byte_code,
  //     static_cast<NodeFunctionEquation *>(node.storage)->stack_size);

  builder.construct_and_set_matching_fn<EquationEvaluateFunction>(node);
}

static void node_rna(StructRNA *srna) {}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, FN_NODE_EQUATION, "Evaluate and equation", NODE_CLASS_CONVERTER);
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(&ntype);
  blender::bke::node_type_storage(
      &ntype, "NodeFunctionEquation", node_free_standard_storage, node_copy_standard_storage);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_equation_cc
