/* SPDX-FileCopyrightText: 2025 Blender Authors
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

struct ExpressionItemsAccessor {
  using ItemT = NodeExpressionItem;
  static StructRNA *item_srna;
  static int node_type;
  static int item_dna_type;
  static constexpr const char *node_idname = "GeometryNodeExpression";
  static constexpr bool has_type = true;
  static constexpr bool has_name = true;
  static constexpr bool has_single_identifier_str = true;
  struct operator_idnames {
    static constexpr const char *add_item = "NODE_OT_expression_item_add";
    static constexpr const char *remove_item = "NODE_OT_expression_item_remove";
    static constexpr const char *move_item = "NODE_OT_expression_item_move";
  };
  struct ui_idnames {
    static constexpr const char *list = "NODE_UL_expression_items";
  };
  struct rna_names {
    static constexpr const char *items = "expression_items";
    static constexpr const char *active_index = "active_index";
  };

  static bool supports_socket_type(const eNodeSocketDatatype socket_type)
  {
    return ELEM(socket_type,
                SOCK_FLOAT,
                SOCK_VECTOR,
                //                SOCK_RGBA,
                SOCK_BOOLEAN,
                //                SOCK_ROTATION,
                //                SOCK_MATRIX,
                SOCK_INT);
  }

  static socket_items::SocketItemsRef<NodeExpressionItem> get_items_from_node(bNode &node)
  {
    auto *storage = static_cast<NodeGeometryExpression *>(node.storage);
    return {&storage->socket_items.items_array,
            &storage->socket_items.items_num,
            &storage->socket_items.active_index};
  }

  static void copy_item(const NodeExpressionItem &src, NodeExpressionItem &dst)
  {
    dst = src;
    dst.name = BLI_strdup_null(dst.name);
    dst.socket_type = dst.socket_type;
    dst.description = BLI_strdup_null(dst.description);
  }

  static void destruct_item(NodeExpressionItem *item)
  {
    MEM_SAFE_FREE(item->name);
    MEM_SAFE_FREE(item->description);
  }

  static void blend_write_item(BlendWriter *writer, const ItemT &item);
  static void blend_read_data_item(BlendDataReader *reader, ItemT &item);
  static eNodeSocketDatatype get_socket_type(const NodeExpressionItem &item)
  {
    return eNodeSocketDatatype(item.socket_type);
  }

  static char **get_name(NodeExpressionItem &item)
  {
    return &item.name;
  }

  static void init_with_socket_type_and_name(bNode &node,
                                             NodeExpressionItem &item,
                                             const eNodeSocketDatatype socket_type,
                                             const char *name)
  {
    auto *storage = static_cast<NodeGeometryExpression *>(node.storage);
    item.socket_type = socket_type;
    item.identifier = storage->socket_items.next_identifier++;

    // If the given name is unique, keep it
    const char *new_name = BLI_strdup(name);
    if (!is_unique_name(node, name)) {
      MEM_SAFE_FREE(new_name);
      new_name = get_new_unique_name(node, name);
    }

    socket_items::set_item_name_and_make_unique<ExpressionItemsAccessor>(node, item, new_name);

    MEM_SAFE_FREE(new_name);
  }

  static std::string socket_identifier_for_item(const NodeExpressionItem &item)
  {
    return "Item_" + std::to_string(item.identifier);
  }

  static bool is_unique_name(const bNode &node, const char *new_name)
  {
    auto *storage = static_cast<NodeGeometryExpression *>(node.storage);
    for (auto it : storage->socket_items.items()) {
      if (it.name && STREQ(new_name, it.name)) {
        return false;
      }
    }
    return true;
  }

  static const char *get_new_unique_name(const bNode &node, const char *base_name)
  {
    // if we're based off a one character name, then get next free charater
    if (strlen(base_name) == 1) {
      char first_char = base_name[0];
      char new_char = first_char;
      char *new_name = (char *)MEM_mallocN(2, __func__);
      new_name[1] = '\0';

      do {
        new_char++;
        if (new_char == (char)('Z' + 1))
          new_char = 'A';
        if (new_char == (char)('z' + 1))
          new_char = 'a';
        new_name[0] = new_char;
      } while (!is_unique_name(node, new_name) && new_char != first_char);
      return new_name;
    }
    else {
      // other wise append or increament a number
      size_t base_name_len = strlen(base_name);
      char *new_name = (char *)MEM_mallocN(base_name_len + 6, __func__);
      strncpy(new_name, base_name, base_name_len);

      int i = 0;
      char *num_buf = &new_name[strlen(base_name)];
      // If the name already ends in a number, use it as start i
      const char *last = &base_name[base_name_len - 1];
      while (last >= base_name && std::isdigit(*last))
        last--;
      if (last != &base_name[base_name_len - 1]) {
        i = atoi(last + 1);
        num_buf = new_name + (last + 1 - base_name);
      }
      if (i >= 99999)
        i = 0;

      do {
        i++;
        itoa(i, num_buf, 10);
      } while (!is_unique_name(node, new_name) && i < 99999);
      return new_name;
    }
  }
};
}  // namespace blender::nodes
