/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "NOD_socket_items.hh"

namespace blender::nodes {

/**
 * Makes it possible to use various functions (e.g. the ones in `NOD_socket_items.hh`) for menu
 * switch node items.
 */

struct EquationItemsAccessor {
  using ItemT = NodeEquationItem;
  static StructRNA *item_srna;
  static int node_type;
  static int item_dna_type;
  static constexpr const char *node_idname = "FunctionNodeEquation";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  static constexpr bool has_single_identifier_str = true;
  struct operator_idnames {
    static constexpr const char *add_item = "NODE_OT_equation_item_add";
    static constexpr const char *remove_item = "NODE_OT_equation_item_remove";
    static constexpr const char *move_item = "NODE_OT_equation_item_move";
  };
  struct ui_idnames {
    static constexpr const char *list = "NODE_UL_equation_items";
  };
  struct rna_names {
    static constexpr const char *items = "equation_items";
    static constexpr const char *active_index = "active_index";
  };

  static bool supports_socket_type(const eNodeSocketDatatype socket_type)
  {
    return ELEM(socket_type,
                SOCK_FLOAT,
                //                SOCK_VECTOR,
                //                SOCK_RGBA,
                //                SOCK_BOOLEAN,
                //                SOCK_ROTATION,
                //                SOCK_MATRIX,
                SOCK_INT);
  }

  static socket_items::SocketItemsRef<NodeEquationItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeFunctionEquation *>(node.storage);
    return {&storage->socket_items.items_array,
            &storage->socket_items.items_num,
            &storage->socket_items.active_index};
  }

  static void copy_item(const NodeEquationItem &src, NodeEquationItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
    dst.socket_type = dst.socket_type;
    //    dst.description = BLI_strdup_null(dst.description);
  }

  static void destruct_item(NodeEquationItem *item)
  {
    MEM_SAFE_FREE(item->name);
    //    MEM_SAFE_FREE(item->description);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);

  static eNodeSocketDatatype get_socket_type(const NodeEquationItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeEquationItem &item)
  {
    return &item.name;
  }

  /*
  static void init_with_name(bNode &node, NodeEnumItem &item, const char *name)
  {
    auto *storage = static_cast<NodeMenuSwitch *>(node.storage);
    item.identifier = storage->enum_definition.next_identifier++;
    socket_items::set_item_name_and_make_unique<EquationItemsAccessor>(node, item, name);
  }
  */

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeEquationItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeFunctionEquation *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->socket_items.next_identifier++;
    socket_items::set_item_name_and_make_unique<EquationItemsAccessor>(node, item, name);
  }

  static std::string socket_identifier_for_item(const NodeEquationItem &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }
};

}  // namespace blender::nodes
