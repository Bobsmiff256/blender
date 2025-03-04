/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "NOD_geo_Expression.hh"
#include "NOD_node_declaration.hh"
#include "node_geometry_util.hh"

#include "BLI_utildefines.h"
#include "NOD_rna_define.hh"
#include "UI_interface.hh"

#include "NOD_socket_items_ops.hh"
//  #include "UI_resources.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"
#include "corecrt_math_defines.h"

#include "BLO_read_write.hh"
#include <charconv>
#include <locale.h>
#include <stdint.h>

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

namespace blender::nodes::node_geo_expression_cc {

////////////////////////////////////////////////////////////////////////////
// Token
// Struct used for parsing and creating a representation
// of the exprssion for evaluation
////////////////////////////////////////////////////////////////////////////
struct Token {
  enum class TokenType {
    NONE,
    // Constants
    FIRST_CONSTANT,
    CONSTANT_FLOAT = FIRST_CONSTANT,
    CONSTANT_INT,
    // Variables (Inputs)
    FIRST_VARIABLE,
    VARIABLE_FLOAT = FIRST_VARIABLE,
    VARIABLE_INT,
    VARIABLE_BOOL,
    VARIABLE_VEC,
    FIRST_SPECIAL,
    LEFT_PAREN = FIRST_SPECIAL,
    RIGHT_PAREN,
    COMMA,
    // Operators
    FIRST_OPERATOR,
    OPERATOR_UNARY_MINUS = FIRST_OPERATOR,
    OPERATOR_UNARY_MINUS_INT,
    OPERATOR_UNARY_MINUS_VEC,
    OPERATOR_UNARY_NOT,
    OPERATOR_PLUS,
    OPERATOR_PLUS_INT,
    OPERATOR_PLUS_VEC,
    OPERATOR_MINUS,
    OPERATOR_MINUS_INT,
    OPERATOR_MINUS_VEC,
    OPERATOR_MULTIPLY,
    OPERATOR_MULTIPLY_INT,
    OPERATOR_MULTIPLY_FLOAT_VEC,
    OPERATOR_MULTIPLY_VEC_FLOAT,
    OPERATOR_DIVIDE,
    OPERATOR_DIVIDE_INT,
    OPERATOR_DIVIDE_VEC_FLOAT,
    OPERATOR_POWER,
    OPERATOR_POWER_INT,
    OPERATOR_MODULO,
    OPERATOR_MODULO_INT,
    OPERATOR_EQUAL,
    OPERATOR_EQUAL_INT,
    OPERATOR_EQUAL_VEC,
    OPERATOR_NOT_EQUAL,
    OPERATOR_NOT_EQUAL_INT,
    OPERATOR_NOT_EQUAL_VEC,
    OPERATOR_GREATER,
    OPERATOR_GREATER_INT,
    OPERATOR_GREATER_EQUAL,
    OPERATOR_GREATER_EQUAL_INT,
    OPERATOR_LESS,
    OPERATOR_LESS_INT,
    OPERATOR_LESS_EQUAL,
    OPERATOR_LESS_EQUAL_INT,
    FIRST_BOOLEAN_OPERATOR,
    OPERATOR_AND = FIRST_BOOLEAN_OPERATOR,
    OPERATOR_OR,
    FIRST_POSTFIX_OPERATOR,
    OPERATOR_GET_MEMBER_VEC = FIRST_POSTFIX_OPERATOR,
    // Functions
    FIRST_FUNCTION,
    FUNCTION_SQUARE_ROOT = FIRST_FUNCTION,
    FUNCTION_SINE,
    FUNCTION_COSINE,
    FUNCTION_TANGENT,
    FUNCTION_ASIN,
    FUNCTION_ACOS,
    FUNCTION_ATAN,
    FUNCTION_ATAN2,
    FUNCTION_MAX,
    FUNCTION_MAX_INT,
    FUNCTION_MIN,
    FUNCTION_MIN_INT,
    FUNCTION_ABS,
    FUNCTION_ABS_INT,
    FUNCTION_SIGN,
    FUNCTION_SIGN_INT,
    FUNCTION_TO_RADIANS,
    FUNCTION_TO_DEGREES,
    FUNCTION_VECTOR,
    FUNCTION_NOT,
    FUNCTION_LOG,
    FUNCTION_LN,
    FUNCTION_POW,
    FUNCTION_EXP,
    FUNCTION_IF,
    FUNCTION_IF_INT,
    FUNCTION_IF_VEC,
    FUNCTION_CEIL,
    FUNCTION_FLOOR,
    FUNCTION_FRAC,
    FUNCTION_ROUND,
    FUNCTION_TRUNCATE,
    FUNCTION_COMPARE,
    FUNCTION_COMPARE_VEC,
    FUNCTION_DOT,
    FUNCTION_CROSS,
    FUNCTION_NORMALIZE,
    FUNCTION_LENGTH,
    FUNCTION_LENGTH2,
    CONVERT_INT_FLOAT,
    CONVERT_FLOAT_INT,
    NUM
  };

  // Describes the type of argument on the stack
  // We keep track of types while creating the program
  // issuing modified token types to always use the correct types
  // so that no checking is required during evaluation
  enum class eValueType { NONE, FLOAT, INT, VEC, NUM };

  // Use some abreviations otherwise format on save messes up format

  TokenType type;
  int value;

  // constructors
 public:
  inline Token() : type(TokenType::NONE), value(0) {}
  inline Token(TokenType t, int param) : type(t), value(param) {}
  inline Token(TokenType t, float param) : type(t)
  {
    // Store the float in the int space
    auto fPtr = reinterpret_cast<float *>(&value);
    *fPtr = param;
  }

  inline Token(const Token &other) : type(other.type), value(other.value) {}

  inline bool is_operand() const
  {
    return type >= TokenType::FIRST_CONSTANT && type < TokenType::FIRST_SPECIAL;
  }
  inline bool is_constant() const
  {
    return type >= TokenType::FIRST_CONSTANT && type < TokenType::FIRST_VARIABLE;
  }
  inline bool is_operator() const
  {
    return type >= TokenType::FIRST_OPERATOR && type < TokenType::FIRST_FUNCTION;
  }
  inline bool is_operator_or_function() const
  {
    return type >= TokenType::FIRST_OPERATOR && type < TokenType::NUM;
  }
  inline bool is_postfix_operator() const
  {
    return type >= TokenType::FIRST_POSTFIX_OPERATOR && type < TokenType::NUM;
  }
  inline float get_value_as_float() const
  {
    return *reinterpret_cast<const float *>(&value);
  };

  // Some accessor functions for token info
  inline int precedence() const;
  inline int num_args() const;
  inline eValueType result_type() const;
  static inline eValueType result_type(TokenType t);
  static inline bool is_boolean_op(TokenType t);
};

////////////////////////////////////////////////////////////////////////////
// TokenInfo
// Information about various tokens
////////////////////////////////////////////////////////////////////////////
struct TokenInfo {
  Token::TokenType type;
  const char *name;
  int precedence;
  Token::eValueType result_type;
  int num_args;
  Token::eValueType arg1_type;
  Token::eValueType arg2_type;
  Token::eValueType arg3_type;
};

// Need some abbreviations to stop lines getting too long
// and the auto formatter messing things up
using EV = Token::eValueType;
using T = Token::TokenType;
#define NA EV::NONE

static const TokenInfo const token_info[(int)T::NUM] = {
    {T::NONE, "NONE", 0, NA, 0, NA, NA},
    // Constants
    {T::CONSTANT_FLOAT, "CONST_FLOAT", 0, EV::FLOAT, 0, NA, NA},
    {T::CONSTANT_INT, "CONSTANT_INT", 0, EV::INT, 0, NA, NA},
    // Variables (Inputs)
    {T::VARIABLE_FLOAT, "VARIABLE_FLOAT", 0, EV::FLOAT, 0, NA, NA},
    {T::VARIABLE_INT, "VARIABLE_INT", 0, EV::INT, 0, NA, NA},
    {T::VARIABLE_BOOL, "VARIABLE_BOOL", 0, EV::INT, 0, NA, NA},
    {T::VARIABLE_VEC, "VARIABLE_VECTOR", 0, EV::VEC, 0, NA, NA},
    // Specials
    {T::LEFT_PAREN, "LEFT_PAREN", 0, NA, 0, NA, NA},
    {T::RIGHT_PAREN, "RIGHT_PAREN", 0, NA, 0, NA, NA},
    {T::COMMA, "COMMA", 0, NA, 0, NA, NA},
    // Operators
    {T::OPERATOR_UNARY_MINUS, "OP_UNARY_MINUS_F", 7, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::OPERATOR_UNARY_MINUS_INT, "OP_UNARY_MINUS_I", 7, EV::INT, 1, EV::INT, NA},
    {T::OPERATOR_UNARY_MINUS_VEC, "OP_UNARY_MINUS_V", 7, EV::VEC, 1, EV::VEC, NA},
    {T::OPERATOR_UNARY_NOT, "OP_UNARY_NOT", 7, EV::INT, 1, EV::INT},
    {T::OPERATOR_PLUS, "OP_PLUS_F", 1, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_PLUS_INT, "OP_PLUS_I", 1, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_PLUS_VEC, "OP_PLUS_V", 1, EV::VEC, 2, EV::VEC, EV::VEC},
    {T::OPERATOR_MINUS, "OP_MINUS_F", 1, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_MINUS_INT, "OP_MINUS_I", 1, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_MINUS_VEC, "OP_MINUS_V", 1, EV::VEC, 2, EV::VEC, EV::VEC},
    {T::OPERATOR_MULTIPLY, "OP_MULTIPLY_F", 2, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_MULTIPLY_INT, "OP_MULTIPLY_I", 2, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_MULTIPLY_FLOAT_VEC, "OP_MULTIPLY_FV", 2, EV::VEC, 2, EV::FLOAT, EV::VEC},
    {T::OPERATOR_MULTIPLY_VEC_FLOAT, "OP_MULTIPLY_VF", 2, EV::VEC, 2, EV::VEC, EV::FLOAT},
    {T::OPERATOR_DIVIDE, "OP_DIVIDE_F", 2, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_DIVIDE_INT, "OP_DIVIDE_I", 2, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_DIVIDE_VEC_FLOAT, "OP_DIVIDE_VF", 2, EV::VEC, 2, EV::VEC, EV::FLOAT},
    {T::OPERATOR_POWER, "OP_POWER_F", 8, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_POWER_INT, "OP_POWER_I", 8, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_MODULO, "OP_MODULO_F", 2, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_MODULO_INT, "OP_MODULO_I", 2, EV::INT, 2, EV::INT, EV::INT},
    // Comparison
    {T::OPERATOR_EQUAL, "OP_EQUAL_F", -1, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_EQUAL_INT, "OP_EQUAL_I", -1, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_EQUAL_VEC, "OP_EQUAL_VEC", -1, EV::INT, 2, EV::VEC, EV::VEC},
    {T::OPERATOR_NOT_EQUAL, "OP_NOT_EQUAL_F", -1, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_NOT_EQUAL_INT, "OP_NOT_EQUAL_I", -1, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_NOT_EQUAL_VEC, "OP_NOT_EQUAL_VEC", -1, EV::INT, 2, EV::VEC, EV::VEC},
    {T::OPERATOR_GREATER, "OP_GREATER", 0, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_GREATER_INT, "OP_GREATER_I", 0, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_GREATER_EQUAL, "OP_GREATER_EQUAL", 0, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_GREATER_EQUAL_INT, "OP_GREATER_EQUAL_I", 0, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_LESS, "OP_LESS", 0, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_LESS_INT, "OP_LESS_I", 0, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_LESS_EQUAL, "OP_LESS_EQUAL", 0, EV::INT, 2, EV::FLOAT, EV::FLOAT},
    {T::OPERATOR_LESS_EQUAL_INT, "OP_LESS_EQUAL_INT", 0, EV::INT, 2, EV::INT, EV::INT},
    // Boolean ops
    {T::OPERATOR_AND, "OP_AND", -2, EV::INT, 2, EV::INT, EV::INT},
    {T::OPERATOR_OR, "OP_OR", -3, EV::INT, 2, EV::INT, EV::INT},
    // Postfix ops
    {T::OPERATOR_GET_MEMBER_VEC, "OP_READ_MEMBER_V", 9, EV::FLOAT, 1, EV::VEC, NA},
    // Functions
    {T::FUNCTION_SQUARE_ROOT, "FN_SQUARE_ROOT", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_SINE, "FN_SIN", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_COSINE, "FN_COS", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_TANGENT, "FN_TAN", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_ASIN, "FN_ASIN", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_ACOS, "FN_ACOS", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_ATAN, "FN_ATAN", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_ATAN2, "FN_ATAN2", 9, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_MAX, "FN_MAX_F", 9, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_MAX_INT, "FN_MAX_I", 9, EV::INT, 2, EV::INT, EV::INT},
    {T::FUNCTION_MIN, "FN_MIN_F", 9, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_MIN_INT, "FN_MIN_I", 9, EV::INT, 2, EV::INT, EV::INT},
    {T::FUNCTION_ABS, "FN_ABS", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_ABS_INT, "FN_ABS_INT", 9, EV::INT, 1, EV::INT, NA},
    {T::FUNCTION_SIGN, "FN_SIGN", 9, EV::INT, 1, EV::FLOAT, NA},
    {T::FUNCTION_SIGN_INT, "FN_SIGN_INT", 9, EV::INT, 1, EV::INT, NA},
    {T::FUNCTION_TO_RADIANS, "FN_TO_RADIANS", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_TO_DEGREES, "FN_TO_DEGREES", 9, EV::FLOAT, 1, EV::FLOAT, NA},
    {T::FUNCTION_VECTOR, "FN_VECTOR", 9, EV::VEC, 3, EV::FLOAT, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_NOT, "FUNCTION_NOT", 9, EV::INT, 1, EV::INT},
    {T::FUNCTION_LOG, "FUNCTION_LOG", 9, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_LN, "FUNCTION_LN", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_POW, "FUNCTION_POW", 9, EV::FLOAT, 2, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_EXP, "FUNCTION_EXP", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_IF, "FUNCTION_IF", 9, EV::FLOAT, 3, EV::INT, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_IF_INT, "FUNCTION_IF_I", 9, EV::INT, 3, EV::INT, EV::INT, EV::INT},
    {T::FUNCTION_IF_VEC, "FUNCTION_IF_VEC", 9, EV::VEC, 3, EV::INT, EV::VEC, EV::VEC},
    {T::FUNCTION_CEIL, "FUNCTION_CEIL", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_FLOOR, "FUNCTION_FLOOR", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_FRAC, "FUNCTION_FRAC", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_ROUND, "FUNCTION_ROUND", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_TRUNCATE, "FUNCTION_TRUNCATE", 9, EV::FLOAT, 1, EV::FLOAT},
    {T::FUNCTION_COMPARE, "FUNCTION_COMPARE", 9, EV::INT, 3, EV::FLOAT, EV::FLOAT, EV::FLOAT},
    {T::FUNCTION_COMPARE_VEC, "FUNCTION_COMPARE_VEC", 9, EV::INT, 3, EV::VEC, EV::VEC, EV::FLOAT},
    {T::FUNCTION_DOT, "FUNCTION_DOT_PRODUCT", 9, EV::FLOAT, 2, EV::VEC, EV::VEC},
    {T::FUNCTION_CROSS, "FUNCTION_CROSS_PRODUCT", 9, EV::VEC, 2, EV::VEC, EV::VEC},
    {T::FUNCTION_NORMALIZE, "FUNCTION_NORMALIZE", 9, EV::VEC, 1, EV::VEC},
    {T::FUNCTION_LENGTH, "FUNCTION_LENGTH", 9, EV::FLOAT, 1, EV::VEC},
    {T::FUNCTION_LENGTH2, "FUNCTION_LENGTH_SQUARED", 9, EV::FLOAT, 1, EV::VEC},
    {T::CONVERT_INT_FLOAT, "FN_CONV_I2F", 9, EV::FLOAT, 1, EV::INT, NA},
    {T::CONVERT_FLOAT_INT, "FN_CONV_F2I", 9, EV::INT, 1, EV::FLOAT, NA},
};

#undef NA

inline int Token::precedence() const
{
  return token_info[(int)type].precedence;
}

inline int Token::num_args() const
{
  return token_info[(int)type].num_args;
}

inline Token::eValueType Token::result_type() const
{
  return token_info[(int)type].result_type;
}

inline Token::eValueType Token::result_type(TokenType t)
{
  return token_info[(int)t].result_type;
}
inline bool Token::is_boolean_op(Token::TokenType t)
{
  return t >= Token::TokenType::FIRST_BOOLEAN_OPERATOR &&
         t < Token::TokenType::FIRST_POSTFIX_OPERATOR;
}

#ifndef NDEBUG
// Check that token info is correctly size and has entries in correct order
void token_info_check()
{
  BLI_assert(sizeof(token_info) / sizeof(TokenInfo) == (int)T::NUM);
  for (int t = (int)Token::TokenType::NONE; t < (int)Token::TokenType::NUM; t++) {
    BLI_assert(token_info[t].type == (Token::TokenType)t);

    int num_args = token_info[t].num_args;
    if (num_args >= 1) {
      BLI_assert(token_info[t].arg1_type != EV::NONE);
    }
    if (num_args >= 2) {
      BLI_assert(token_info[t].arg2_type != EV::NONE);
    }
    if (num_args >= 3) {
      BLI_assert(token_info[t].arg3_type != EV::NONE);
    }
  }
}
#endif

// Lookup table to convert funtion names to token types
struct func_lookup {
  const char *name;
  Token::TokenType type;
};
// NOTE name must be all lowercase
constexpr func_lookup func_table[] = {
    {"sin", Token::TokenType::FUNCTION_SINE},
    {"sine", Token::TokenType::FUNCTION_SINE},
    {"cos", Token::TokenType::FUNCTION_COSINE},
    {"cosine", Token::TokenType::FUNCTION_COSINE},
    {"tan", Token::TokenType::FUNCTION_TANGENT},
    {"tangent", Token::TokenType::FUNCTION_TANGENT},
    {"asin", Token::TokenType::FUNCTION_ASIN},
    {"arcsine", Token::TokenType::FUNCTION_ASIN},
    {"acos", Token::TokenType::FUNCTION_ACOS},
    {"arccosine", Token::TokenType::FUNCTION_ACOS},
    {"atan", Token::TokenType::FUNCTION_ATAN},
    {"arctangent", Token::TokenType::FUNCTION_ATAN},
    {"atan2", Token::TokenType::FUNCTION_ATAN2},
    {"max", Token::TokenType::FUNCTION_MAX},
    {"maximum", Token::TokenType::FUNCTION_MAX},
    {"min", Token::TokenType::FUNCTION_MIN},
    {"minimum", Token::TokenType::FUNCTION_MIN},
    {"sqrt", Token::TokenType::FUNCTION_SQUARE_ROOT},
    {"squareroot", Token::TokenType::FUNCTION_SQUARE_ROOT},
    {"square_root", Token::TokenType::FUNCTION_SQUARE_ROOT},
    {"abs", Token::TokenType::FUNCTION_ABS},
    {"absolute", Token::TokenType::FUNCTION_ABS},
    {"sign", Token::TokenType::FUNCTION_SIGN},
    {"toradians", Token::TokenType::FUNCTION_TO_RADIANS},
    {"to_radians", Token::TokenType::FUNCTION_TO_RADIANS},
    {"todegrees", Token::TokenType::FUNCTION_TO_DEGREES},
    {"to_degrees", Token::TokenType::FUNCTION_TO_DEGREES},
    {"vec", Token::TokenType::FUNCTION_VECTOR},
    {"vector", Token::TokenType::FUNCTION_VECTOR},
    {"not", Token::TokenType::FUNCTION_NOT},
    {"log", Token::TokenType::FUNCTION_LOG},
    {"logarithm", Token::TokenType::FUNCTION_LOG},
    {"ln", Token::TokenType::FUNCTION_LN},
    {"pow", Token::TokenType::FUNCTION_POW},
    {"power", Token::TokenType::FUNCTION_POW},
    {"exp", Token::TokenType::FUNCTION_EXP},
    {"exponential", Token::TokenType::FUNCTION_EXP},
    {"if", Token::TokenType::FUNCTION_IF},
    {"ceil", Token::TokenType::FUNCTION_CEIL},
    {"floor", Token::TokenType::FUNCTION_FLOOR},
    {"frac", Token::TokenType::FUNCTION_FRAC},
    {"fraction", Token::TokenType::FUNCTION_FRAC},
    {"round", Token::TokenType::FUNCTION_ROUND},
    {"truncate", Token::TokenType::FUNCTION_TRUNCATE},
    {"trunc", Token::TokenType::FUNCTION_TRUNCATE},
    {"compare", Token::TokenType::FUNCTION_COMPARE},
    {"dot", Token::TokenType::FUNCTION_DOT},
    {"cross", Token::TokenType::FUNCTION_CROSS},
    {"normalize", Token::TokenType::FUNCTION_NORMALIZE},
    {"length", Token::TokenType::FUNCTION_LENGTH},
    {"length2", Token::TokenType::FUNCTION_LENGTH2},
};

// List of possible overloads for operator and function token types
struct overload_set {
  Token::TokenType base, alt1, alt2, alt3, alt4, alt5;
  static const int max_overloads = 5;
};

constexpr const overload_set overloads[] = {
    // Single op functions
    {T::OPERATOR_UNARY_MINUS, T::OPERATOR_UNARY_MINUS_INT, T::OPERATOR_UNARY_MINUS_VEC},
    {T::FUNCTION_ABS, T::FUNCTION_ABS_INT},
    {T::FUNCTION_SIGN, T::FUNCTION_SIGN_INT},
    // Two op functions
    {T::OPERATOR_PLUS, T::OPERATOR_PLUS_INT, T::OPERATOR_PLUS_VEC},
    {T::OPERATOR_MINUS, T::OPERATOR_MINUS_INT, T::OPERATOR_MINUS_VEC},
    {T::OPERATOR_MULTIPLY,
     T::OPERATOR_MULTIPLY_INT,
     T::OPERATOR_MULTIPLY_VEC_FLOAT,
     T::OPERATOR_MULTIPLY_FLOAT_VEC},
    {T::OPERATOR_DIVIDE, T::OPERATOR_DIVIDE_INT, T::OPERATOR_DIVIDE_VEC_FLOAT},
    {T::OPERATOR_POWER, T::OPERATOR_POWER_INT},
    {T::OPERATOR_MODULO, T::OPERATOR_MODULO_INT},
    {T::OPERATOR_EQUAL, T::OPERATOR_EQUAL_INT, T::OPERATOR_EQUAL_VEC},
    {T::OPERATOR_NOT_EQUAL, T::OPERATOR_NOT_EQUAL_INT, T::OPERATOR_NOT_EQUAL_VEC},
    {T::OPERATOR_GREATER, T::OPERATOR_GREATER_INT},
    {T::OPERATOR_GREATER_EQUAL, T::OPERATOR_GREATER_EQUAL_INT},
    {T::OPERATOR_LESS, T::OPERATOR_LESS_INT},
    {T::OPERATOR_LESS_EQUAL, T::OPERATOR_LESS_EQUAL_INT},
    {T::FUNCTION_MAX, T::FUNCTION_MAX_INT},
    {T::FUNCTION_MIN, T::FUNCTION_MIN_INT},
    // Three op functions
    {T::FUNCTION_IF, T::FUNCTION_IF_INT, T::FUNCTION_IF_VEC},
    {T::FUNCTION_COMPARE, T::FUNCTION_COMPARE_VEC}};

////////////////////////////////////////////////////////////////////////////
// TokenQueue
// Class that maintains an array of tokens
////////////////////////////////////////////////////////////////////////////
class TokenQueue {
  Vector<Token, 50, GuardedAllocator> buffer_{};

 public:
  TokenQueue() {}

  void add_token(Token::TokenType t, int param)
  {
    buffer_.append(Token(t, param));
  }
  void add_token(Token::TokenType t, float param)
  {
    buffer_.append(Token(t, param));
  }

  void add_token(Token t)
  {
    buffer_.append(t);
  }

  const int element_count() const
  {
    return buffer_.size();
  }

  Token at(int index) const
  {
    return buffer_[index];
  }

  void clear()
  {
    buffer_.clear();
  }

  const bool is_empty() const
  {
    return buffer_.is_empty();
  }

  void discard_last()
  {
    buffer_.pop_last();
  }

  Token last() const
  {
    return buffer_.last();
  }

  void print() const
  {
    printf("%i Tokens:\n", (int)buffer_.size());
    for (int i = 0; i < buffer_.size(); i++) {
      Token t = buffer_[i];
      if (t.is_operand())
        printf("%s(%d) ", token_info[(int)t.type].name, t.value);
      else
        printf("%s ", token_info[(int)t.type].name);

      if (i > 0 && (i % 8) == 0)
        printf("\n");
    }
    printf("\n");
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionParser
// Class that parses the expression into a buffer of tokens
////////////////////////////////////////////////////////////////////////////
class ExpressionParser {
 public:
  const Vector<const char *> *input_names_;
  const Vector<short> *input_types_;
  const char *error_msg_ = "";
  int error_pos_ = -1;

  ExpressionParser(const Vector<const char *> *input_names, const Vector<short> *input_types)
      : input_names_(input_names), input_types_(input_types)
  {
  }

  bool parse(const char *expression, TokenQueue &buffer, const char *&error_msg, int &error_pos)
  {
    error_msg_ = "";
    error_pos_ = 0;

    int read_pos = 0;
    bool ok = parse_expression(expression, read_pos, buffer, false);
    error_msg = error_msg_;
    error_pos = error_pos_;
    return ok;
  }

  void set_error_if_none(const char *msg, int position)
  {
    if (error_msg_ == nullptr || error_msg_ == "") {
      error_msg_ = msg;
      error_pos_ = position;
    }
  }

  bool parse_expression(const std::string &input,
                        int &read_pos,
                        TokenQueue &output,
                        bool terminate_on_close_parens,
                        bool terminate_on_comma = false)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return false;

    if (!parse_operand_or_unary(input, read_pos, output)) {
      set_error_if_none(TIP_("Expected an operand"), read_pos);
      return false;
    }

    while (true) {
      // If we've reached the end of the input or a parenthesized expression
      // then we have a valid exprssion
      skip_white_space(input, read_pos);
      if (read_pos == input.length())
        return true;
      if (terminate_on_close_parens && input.at(read_pos) == ')')
        return true;
      if (terminate_on_comma && input.at(read_pos) == ',')
        return true;

      // Expect an operator and another operand
      if (!parse_operator(input, read_pos, output)) {
        set_error_if_none(TIP_("Expected an operator"), read_pos);
        return false;
      }
      // expect another operand after an operator, unless it was postifx
      if (!output.last().is_postfix_operator()) {
        if (!parse_operand_or_unary(input, read_pos, output)) {
          set_error_if_none(TIP_("Expected an operand after operator"), read_pos);
          return false;
        }
      }
    }

    return true;
  }

  bool parse_operand_or_unary(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return false;

    // Check for unary operators
    Token::TokenType unary_op = Token::TokenType::NONE;
    // Check for unary minus operator. Skip if followed by digit
    if (input.at(read_pos) == '-' && read_pos < input.length() - 1 &&
        !isdigit(input.at(read_pos + 1)))
    {
      unary_op = Token::TokenType::OPERATOR_UNARY_MINUS;
    }
    else if (input.at(read_pos) == '!') {
      unary_op = Token::TokenType::OPERATOR_UNARY_NOT;
    }

    if (unary_op != Token::TokenType::NONE) {
      output.add_token(unary_op, 0);
      read_pos++;
      if (!parse_operand(input, read_pos, output)) {
        set_error_if_none(TIP_("Expected operand after unary operator"), read_pos);
        return false;
      }
    }
    else
      return parse_operand(input, read_pos, output);
  }

  bool parse_operand(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    if (input.at(read_pos) == '(') {
      int paren_start = read_pos;
      output.add_token(Token::TokenType::LEFT_PAREN, 0);
      read_pos++;

      if (!parse_expression(input, read_pos, output, true)) {
        set_error_if_none(TIP_("Expected expression after parenthesis"), read_pos);
        return false;
      }

      if (!parse_right_paren(input, read_pos, output)) {
        error_msg_ = "Unclosed parenthesis";
        error_pos_ = paren_start;
        return false;
      }

      return true;
    }
    else {
      if (next_input_is_function_name(input, read_pos))
        return parse_function(input, read_pos, output);
      if (parse_number(input, read_pos, output))
        return true;
      error_msg_ = "";  // discard error message from attempting to read number
      if (read_variable_name_size(input, read_pos) != 0)
        return parse_variable(input, read_pos, output);

      set_error_if_none(TIP_("Expected a constant, variable or function"), read_pos);
      return false;
    }
  }

  bool parse_function(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int start_read_pos = read_pos;

    // Read the function name
    auto function_op = read_function_op(input, read_pos);
    if (function_op == Token::TokenType::NONE) {
      set_error_if_none(TIP_("Unknown function name"), start_read_pos);
      return false;
    }

    output.add_token(function_op, 0);
    int num_args = token_info[(int)function_op].num_args;

    // Now expect a left paren
    if (!parse_left_paren(input, read_pos, output)) {
      // set_error_if_none(TIP_("Expected '(' after function name"), start_read_pos);
      read_pos = start_read_pos;
      return false;
    }

    // Now expect an expression
    start_read_pos = read_pos;
    if (!parse_expression(input, read_pos, output, true, num_args > 1)) {
      read_pos = start_read_pos;
      return false;
    }

    // expect commas and further expressions for multi-operand functions
    int expected_args = num_args - 1;
    while (expected_args--) {
      if (!parse_comma(input, read_pos, output)) {
        read_pos = start_read_pos;
        return false;
      }
      if (!parse_expression(input, read_pos, output, true, expected_args > 0)) {
        if (num_args == 2)
          set_error_if_none(TIP_("Expected 2 arguments to function"), start_read_pos);
        else
          set_error_if_none(TIP_("Expected 3 arguments to function"), start_read_pos);
        read_pos = start_read_pos;
        return false;
      }
    };

    // Expect a right param
    if (!parse_right_paren(input, read_pos, output))
      return false;
  }

  bool parse_left_paren(const std::string &input, int &read_pos, TokenQueue &output)
  {
    bool fail = false;

    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      fail = true;

    if (!fail && input.at(read_pos) == '(') {
      output.add_token(Token::TokenType::LEFT_PAREN, 0);
      read_pos++;
      return true;
    }
    else {
      set_error_if_none(TIP_("Expected ("), read_pos);
      return false;
    }
  }
  bool parse_right_paren(const std::string &input, int &read_pos, TokenQueue &output)
  {
    bool fail = false;
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      fail = true;

    if (!fail && input.at(read_pos) == ')') {
      output.add_token(Token::TokenType::RIGHT_PAREN, 0);
      read_pos++;
      return true;
    }
    else {
      set_error_if_none(TIP_("Expected )"), read_pos);
      return false;
    }
  }
  bool parse_comma(const std::string &input, int &read_pos, TokenQueue &output)
  {
    bool fail = false;
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      fail = true;

    if (!fail && input.at(read_pos) == ',') {
      output.add_token(Token::TokenType::COMMA, 0);
      read_pos++;
      return true;
    }
    else {
      set_error_if_none(TIP_("Expected ','"), read_pos);
      return false;
    }
  }

  bool parse_operator(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int start_read_pos = read_pos;
    auto op = read_operator_op(input, read_pos);
    if (op == Token::TokenType::NONE) {
      return false;
    }

    if (op == Token::TokenType::OPERATOR_GET_MEMBER_VEC) {
      // This op must be followed directly by a field name
      int field_offset = read_member_offset(input, read_pos);
      if (field_offset == -1) {
        read_pos = start_read_pos;
        set_error_if_none(TIP_("Expected member name directly after \".\""), read_pos);
        return false;
      }
      output.add_token(op, field_offset);
    }
    else
      output.add_token(op, 0);

    return true;
  }

  bool parse_number(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    auto sub_string = input.substr(read_pos, input.length() - read_pos);
    auto last = sub_string.data() + sub_string.size();

    // See if we can read it as either a float or an int, and pick whichever uses more characters
    float f = 0.0;
    auto [fptr, fec] = std::from_chars(sub_string.data(), last, f);
    int x = 0;
    auto [iptr, iec] = std::from_chars(sub_string.data(), last, x);

    if (iec == std::errc{} || fec == std::errc{}) {
      const char *ptr;
      if (iec == std::errc{} && (fec != std::errc{} || iptr >= fptr)) {
        output.add_token(Token::TokenType::CONSTANT_INT, x);
        ptr = iptr;
      }
      else {
        output.add_token(Token::TokenType::CONSTANT_FLOAT, f);
        ptr = fptr;
      }
      if (ptr == last)
        read_pos += sub_string.length();
      else
        read_pos += ptr - sub_string.data();

      return true;
    }

    set_error_if_none(TIP_("Invalid number"), read_pos);
    return false;
  }

  bool is_special_const(const std::string &var_name, float &val) const
  {
    if (var_name.length() == 2 && (var_name[0] == 'p' || var_name[0] == 'P') &&
        (var_name[1] == 'i' || var_name[1] == 'I'))
    {
      val = M_PI;
      return true;
    }
    if (var_name.length() == 3 && (var_name[0] == 't' || var_name[0] == 'T') &&
        (var_name[1] == 'a' || var_name[1] == 'A') && (var_name[2] == 'u' || var_name[2] == 'U'))
    {
      val = M_PI * 2;
      return true;
    }

    return false;
  }

  bool parse_variable(const std::string &input, int &read_pos, TokenQueue &output)
  {
    skip_white_space(input, read_pos);

    if (read_pos == input.length())
      return false;

    int name_len = read_variable_name_size(input, read_pos);
    if (name_len == 0) {
      set_error_if_none(TIP_("Expected a variable name"), read_pos);
      return false;
    }
    std::string var_name = input.substr(read_pos, name_len);

    // Check if it's a special named constant
    float special_const_value;
    if (is_special_const(var_name, special_const_value)) {
      output.add_token(Token::TokenType::CONSTANT_FLOAT, special_const_value);
      read_pos += name_len;
      return true;
    }

    // Check that variable actually exists
    int input_idx = -1;
    for (int i = 0; i < input_names_->size(); i++) {
      if ((*input_names_)[i] == var_name) {
        input_idx = i;
        break;
      }
    }

    if (input_idx == -1) {
      set_error_if_none(TIP_("Unknown input name"), read_pos);
      return false;
    }

    read_pos += name_len;
    switch ((*input_types_)[input_idx]) {
      case eNodeSocketDatatype::SOCK_BOOLEAN:
        output.add_token(Token::TokenType::VARIABLE_BOOL, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_INT:
        output.add_token(Token::TokenType::VARIABLE_INT, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_FLOAT:
        output.add_token(Token::TokenType::VARIABLE_FLOAT, input_idx);
        break;
      case eNodeSocketDatatype::SOCK_VECTOR:
        output.add_token(Token::TokenType::VARIABLE_VEC, input_idx);
        break;
      default:
        BLI_assert_unreachable();
    }
    return true;
  }

  Token::TokenType read_operator_op(const std::string &input, int &read_pos)
  {
    skip_white_space(input, read_pos);
    if (read_pos == input.length())
      return Token::TokenType::NONE;

    // Try single character ops
    char op_char = input.at(read_pos++);

    switch (op_char) {
      case '+':
        return Token::TokenType::OPERATOR_PLUS;
      case '-':
        return Token::TokenType::OPERATOR_MINUS;
      case '*':
        return Token::TokenType::OPERATOR_MULTIPLY;
      case '/':
        return Token::TokenType::OPERATOR_DIVIDE;
      case '^':
        return Token::TokenType::OPERATOR_POWER;
      case '%':
        return Token::TokenType::OPERATOR_MODULO;
      case '.':
        return Token::TokenType::OPERATOR_GET_MEMBER_VEC;
    }

    read_pos--;

    // Try 2 character ops
    if (input.length() - read_pos < 2) {
      return Token::TokenType::NONE;
    }
    std::string two_char_op = input.substr(read_pos, 2);
    read_pos += 2;
    if (two_char_op == "==")
      return Token::TokenType::OPERATOR_EQUAL;
    if (two_char_op == "!=")
      return Token::TokenType::OPERATOR_NOT_EQUAL;
    if (two_char_op == ">=")
      return Token::TokenType::OPERATOR_GREATER_EQUAL;
    if (two_char_op == "<=")
      return Token::TokenType::OPERATOR_LESS_EQUAL;
    if (two_char_op == "or")
      return Token::TokenType::OPERATOR_OR;
    if (two_char_op == "OR")
      return Token::TokenType::OPERATOR_OR;
    if (two_char_op == "||")
      return Token::TokenType::OPERATOR_OR;
    if (two_char_op == "&&")
      return Token::TokenType::OPERATOR_AND;

    // Try the single char ops that are also start of two char ops
    read_pos -= 1;
    switch (op_char) {
      case '>':
        return Token::TokenType::OPERATOR_GREATER;
      case '<':
        return Token::TokenType::OPERATOR_LESS;
      case '=':
        return Token::TokenType::OPERATOR_EQUAL;
    }

    // Try three character ops
    read_pos -= 1;
    if (input.length() - read_pos < 3) {
      return Token::TokenType::NONE;
    }
    std::string three_char_op = input.substr(read_pos, 3);
    read_pos += 3;
    if (three_char_op == "and" || three_char_op == "AND")
      return Token::TokenType::OPERATOR_AND;

    return Token::TokenType::NONE;
  }

  int read_member_offset(const std::string &input, int &read_pos)
  {
    // Note, don't skip whitespace
    if (read_pos == input.length())
      return -1;

    const char name = input.at(read_pos);
    read_pos++;
    if (name == 'x' || name == 'X')
      return 2;
    if (name == 'y' || name == 'Y')
      return 1;
    if (name == 'z' || name == 'Z')
      return 0;

    read_pos--;  // restore read pos before returning error
    return -1;
  }

  bool next_input_is_function_name(const std::string &input, int read_pos)
  {
    // No need to save read_pos as it's passed by value not reference
    return read_function_op(input, read_pos) != Token::TokenType::NONE;
  }

  Token::TokenType read_function_op(const std::string &input, int &read_pos)
  {
    skip_white_space(input, read_pos);
    int paren_pos = input.find('(', read_pos);
    if (paren_pos == -1)
      return Token::TokenType::NONE;

    // Get string up to opening paren and convert to lowercase
    std::string func_name = input.substr(read_pos, paren_pos - read_pos);
    for (auto &c : func_name) {
      c = tolower(c);
    }

    static int num_func_names = sizeof(func_table) / sizeof(func_lookup);
    for (int i = 0; i < num_func_names; i++) {
      if (func_table[i].name == func_name) {
        read_pos = paren_pos;
        return func_table[i].type;
      }
    }

    return Token::TokenType::NONE;
  }

  void skip_white_space(const std::string &input, int &read_pos) const
  {
    while (read_pos < input.length() && isspace(input.at(read_pos)))
      read_pos++;
  }

  // Returns the number of characters that constitute a valid variable name
  // Doesn't adjust read_pos
  // Only checks for a syntacically valid name. Doesn't check if such a variable exists
  // returns 0 if no valid name
  int read_variable_name_size(const std::string &input, int &read_pos) const
  {
    int temp_read_pos = read_pos;
    skip_white_space(input, temp_read_pos);
    if (temp_read_pos == input.length())
      return 0;

    // Check if the first character is valid
    char first = input.at(temp_read_pos++);
    if (first != '_' && !isalpha(first))
      return 0;

    while (temp_read_pos < input.length()) {
      char c = input.at(temp_read_pos);
      if (c != '_' && !isalpha(c) && !isdigit(c))
        break;
      temp_read_pos++;
    }

    return temp_read_pos - read_pos;
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionProgram
// class that holds a representation of the expression for evaluation
// Creates and evaluates the representation
////////////////////////////////////////////////////////////////////////////
class ExpressionProgram {

  using TokenType = Token::TokenType;
  using eValueType = Token::eValueType;

  bool program_valid_ = false;
  TokenQueue program_buffer_;
  static constexpr int MAX_STACK = 100;

 public:
  const Vector<const char *> *input_names_;
  const Vector<short> *input_types_;
  eNodeSocketDatatype output_type_;

 public:
  ExpressionProgram(const Vector<const char *> *input_names,
                    const Vector<short> *input_types,
                    eNodeSocketDatatype output_type)
      : input_names_(input_names), input_types_(input_types), output_type_(output_type)
  {
  }

  bool create_program(const std::string &expression, std::string &error_msg)
  {
    program_valid_ = false;

    // Try to parse the expression
    TokenQueue parse_buffer;
    ExpressionParser parser(input_names_, input_types_);
    const char *parser_error;
    int error_pos;
    bool ok = parser.parse(expression.c_str(), parse_buffer, parser_error, error_pos);

    // Report parsing errors
    if (!ok) {
      // Combine the error message with part of the expression from the error location to give
      // final message
      error_msg = std::move(std::string(parser_error));
      int chars_after_error = expression.size() - error_pos;
      if (chars_after_error == 0 && error_pos > 0) {
        error_pos--;
        chars_after_error++;
      }
      auto expPart = expression.substr(error_pos, chars_after_error);
      error_msg += "\n" + expPart;
      goto exit;
    }

    // Rearrange the raw parsed tokens into a program that can be evaluated
    if (!create_postfix_program(parse_buffer, program_buffer_, error_msg)) {
      goto exit;
    }

    program_valid_ = true;
    goto exit;

  exit:
    // These don't need to persist after this method has run
    input_names_ = nullptr;
    input_types_ = nullptr;
    return program_valid_;
  }

  inline int stack_space(eValueType type)
  {
    return type == eValueType::VEC ? 3 : 1;
  }

  static bool check_arguments_match(Token::TokenType func, Token::eValueType arg_type)
  {
    if (token_info[(int)func].arg1_type != arg_type)
      return false;
    return true;
  }

  static bool check_arguments_match(Token::TokenType func,
                                    Token::eValueType arg1_type,
                                    Token::eValueType arg2_type)
  {
    if (token_info[(int)func].arg1_type != arg1_type)
      return false;
    if (token_info[(int)func].arg2_type != arg2_type)
      return false;
    return true;
  }

  static bool check_arguments_match(Token::TokenType func,
                                    Token::eValueType arg1_type,
                                    Token::eValueType arg2_type,
                                    Token::eValueType arg3_type)
  {
    if (token_info[(int)func].arg1_type != arg1_type)
      return false;
    if (token_info[(int)func].arg2_type != arg2_type)
      return false;
    if (token_info[(int)func].arg3_type != arg3_type)
      return false;
    return true;
  }

  static const overload_set *find_overloads(TokenType t)
  {
    const size_t overload_table_size = sizeof(overloads) / sizeof(overload_set);
    for (int i = 0; i < overload_table_size; i++) {
      if (overloads[i].base == t) {
        return &overloads[i];
      }
    }

    return nullptr;
  }

  // Find the correct operator or function token for a particular argument type
  static TokenType get_op_version_for_type(TokenType base_type, eValueType arg_type)
  {
    // Check if base type args are correct
    if (check_arguments_match(base_type, arg_type))
      return base_type;

    // Find if there is an overload list for this type
    const overload_set *overload_list = find_overloads(base_type);
    if (!overload_list)
      return TokenType::NONE;

    // Check overloads
    for (int i = 0; i < overload_set::max_overloads; i++) {
      TokenType alt = i == 0 ? overload_list->alt1 :
                      i == 1 ? overload_list->alt2 :
                      i == 2 ? overload_list->alt3 :
                      i == 3 ? overload_list->alt4 :
                               overload_list->alt5;
      if (alt == TokenType::NONE)
        return TokenType::NONE;
      if (check_arguments_match(alt, arg_type))
        return alt;
    }

    // No conversion found
    return TokenType::NONE;
  }

  // Find the correct operator or function token for a particular argument pair
  static TokenType get_op_version_for_type(TokenType base_type,
                                           eValueType arg1_type,
                                           eValueType arg2_type)
  {
    // Check if base type args are correct
    if (check_arguments_match(base_type, arg1_type, arg2_type))
      return base_type;

    // Find if there is an overload list for this type
    const overload_set *overload_list = find_overloads(base_type);
    if (!overload_list)
      return TokenType::NONE;

    // Check overloads
    for (int i = 0; i < overload_set::max_overloads; i++) {
      TokenType alt = i == 0 ? overload_list->alt1 :
                      i == 1 ? overload_list->alt2 :
                      i == 2 ? overload_list->alt3 :
                      i == 3 ? overload_list->alt4 :
                               overload_list->alt5;
      if (alt == TokenType::NONE)
        return TokenType::NONE;
      if (check_arguments_match(alt, arg1_type, arg2_type))
        return alt;
    }

    // No conversion found
    return TokenType::NONE;
  }

  // Find the correct operator or function token for a particular argument type
  static TokenType get_op_version_for_type(TokenType base_type,
                                           eValueType arg1_type,
                                           eValueType arg2_type,
                                           eValueType arg3_type)
  {
    // Check if base type args are correct
    if (check_arguments_match(base_type, arg1_type, arg2_type, arg3_type))
      return base_type;

    // Find if there is an overload list for this type
    const overload_set *overload_list = find_overloads(base_type);
    if (!overload_list)
      return TokenType::NONE;

    // Check overloads
    for (int i = 0; i < overload_set::max_overloads; i++) {
      TokenType alt = i == 0 ? overload_list->alt1 :
                      i == 1 ? overload_list->alt2 :
                      i == 2 ? overload_list->alt3 :
                      i == 3 ? overload_list->alt4 :
                               overload_list->alt5;
      if (alt == TokenType::NONE)
        return TokenType::NONE;
      if (check_arguments_match(alt, arg1_type, arg2_type, arg3_type))
        return alt;
    }

    // No conversion found
    return TokenType::NONE;
  }

  // if allowed_implicit_only is true only return conversions op for conversions we want to do
  // implicitly
  TokenType get_type_conversion_op(eValueType from_type,
                                   eValueType to_type,
                                   bool allowed_implicit_only = true)
  {

    if (from_type == eValueType::INT && to_type == eValueType::FLOAT)
      return TokenType::CONVERT_INT_FLOAT;

    // No more implicit conversions
    if (allowed_implicit_only)
      return TokenType::NONE;

    if (from_type == eValueType::FLOAT && to_type == eValueType::INT)
      return TokenType::CONVERT_FLOAT_INT;

    // No suitable conversion
    return TokenType::NONE;
  }

  void output_constant(const Token &t,
                       TokenQueue &output,
                       Vector<Token::eValueType> &stack_type,
                       int &stack_size)
  {
    output.add_token(t);
    stack_size++;  // constants are ints or floats
    if (t.type == Token::TokenType::CONSTANT_FLOAT)
      stack_type.append(eValueType::FLOAT);
    else
      stack_type.append(eValueType::INT);
  }

  void output_variable(const Token &t,
                       TokenQueue &output,
                       Vector<eValueType> &stack_type,
                       int &stack_size)
  {
    output.add_token(t);
    if (t.type == Token::TokenType::VARIABLE_VEC) {
      stack_type.append(eValueType::VEC);
    }
    else {
      if (t.type == Token::TokenType::VARIABLE_INT || t.type == Token::TokenType::VARIABLE_BOOL)
        stack_type.append(eValueType::INT);
      else
        stack_type.append(eValueType::FLOAT);
    }

    // Increase stack size by size of type we just added
    stack_size += stack_space(stack_type.last());
  }

  // Checks if the token can operate with the given arg type (returns token if true)
  // Then tries to find a specialized version of the token for the arg type, and returns that
  // If none found, attempts type conversions, pushing necessary conversions ops into the buffer
  // Returns the actual TokenType to use, and sets arg_type to the new arg type
  // Returns TokenType::NONE if no suitable type conversions are available
  TokenType perform_type_conversion(TokenQueue &output, TokenType type, eValueType &arg_type)
  {
    TokenType specialized_op = get_op_version_for_type(type, arg_type);
    if (specialized_op != TokenType::NONE)
      return specialized_op;

    // See if we can convert int to float
    if (arg_type == eValueType::INT) {
      specialized_op = get_op_version_for_type(type, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Insert conversion op
        arg_type = eValueType::FLOAT;                       // arg type has changed
        return specialized_op;
      }
    }

    return TokenType::NONE;
  }

  // As above for two args
  TokenType perform_type_conversion(TokenQueue &output,
                                    TokenType type,
                                    eValueType &arg1_type,
                                    eValueType &arg2_type)
  {
    TokenType specialized_op = get_op_version_for_type(type, arg1_type, arg2_type);
    if (specialized_op != TokenType::NONE)
      return specialized_op;

    // Check if we can convert arg1_type to arg2_type
    TokenType convert_op = get_type_conversion_op(arg1_type, arg2_type);
    if (convert_op != TokenType::NONE) {
      specialized_op = get_op_version_for_type(type, arg2_type, arg2_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(convert_op,
                         stack_space(arg2_type));  // Convert first arg (1 above stack top)
        arg1_type = arg2_type;                     // set new type
        return specialized_op;
      }
    }

    // Check if we can convert arg2_type to arg1_type
    convert_op = get_type_conversion_op(arg2_type, arg1_type);
    if (convert_op != TokenType::NONE) {
      specialized_op = get_op_version_for_type(type, arg1_type, arg1_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(convert_op, 0);  // Convert second arg (stack top)
        arg2_type = arg1_type;            // set new type
        return specialized_op;
      }
    }

    // See if we can convert int to float
    if (arg1_type == eValueType::INT && arg2_type == eValueType::INT) {
      TokenType specialized_op = get_op_version_for_type(
          type, eValueType::FLOAT, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 1);  // Convert arg1
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Convert arg2
        arg1_type = eValueType::FLOAT;                      // arg type has changed
        arg2_type = eValueType::FLOAT;                      // arg type has changed
        return specialized_op;
      }
    }

    // If we have a vector and an int, try converting int to float
    if (arg1_type == eValueType::INT && arg2_type == eValueType::VEC) {
      TokenType specialized_op = get_op_version_for_type(type, eValueType::FLOAT, arg2_type);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, stack_space(arg2_type));  // Convert arg1
        arg1_type = eValueType::FLOAT;  // arg type has changed
        return specialized_op;
      }
    }
    if (arg1_type == eValueType::VEC && arg2_type == eValueType::INT) {
      TokenType specialized_op = get_op_version_for_type(type, arg1_type, eValueType::FLOAT);
      if (specialized_op != TokenType::NONE) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);  // Convert arg2
        arg2_type = eValueType::FLOAT;                      // arg type has changed
        return specialized_op;
      }
    }

    return TokenType::NONE;
  }

  bool is_scalar(eValueType type)
  {
    return type == eValueType::FLOAT || type == eValueType::INT;
  }

  TokenType perform_type_conversion(TokenQueue &output,
                                    TokenType type,
                                    eValueType &arg1_type,
                                    eValueType &arg2_type,
                                    eValueType &arg3_type)
  {
    TokenType specialized_op = get_op_version_for_type(type, arg1_type, arg2_type, arg3_type);
    if (specialized_op != TokenType::NONE)
      return specialized_op;

    // See if we can convert int to float
    TokenType all_float_op = get_op_version_for_type(
        type, eValueType::FLOAT, eValueType::FLOAT, eValueType::FLOAT);
    if (all_float_op != TokenType::NONE && is_scalar(arg1_type) && is_scalar(arg2_type) &&
        is_scalar(arg3_type))
    {
      if (arg1_type == eValueType::INT) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 2);  // Insert conversion op
        arg1_type = eValueType::FLOAT;                      // arg type has changed
      }
      if (arg2_type == eValueType::INT) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 1);
        arg2_type = eValueType::FLOAT;
      }
      if (arg3_type == eValueType::INT) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);
        arg3_type = eValueType::FLOAT;
      }
      return all_float_op;
    }

    // see if we can convert args 2 and 3 to float
    TokenType last2_float_op = get_op_version_for_type(
        type, arg1_type, eValueType::FLOAT, eValueType::FLOAT);
    if (last2_float_op != TokenType::NONE && is_scalar(arg2_type) && is_scalar(arg3_type)) {
      if (arg2_type == eValueType::INT) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 1);
        arg2_type = eValueType::FLOAT;
      }
      if (arg3_type == eValueType::INT) {
        output.add_token(TokenType::CONVERT_INT_FLOAT, 0);
        arg3_type = eValueType::FLOAT;
      }
      return last2_float_op;
    }

    return TokenType::NONE;
  }

  bool output_op_or_function(const Token &t,
                             TokenQueue &output,
                             Vector<Token::eValueType> &stack_type,
                             int &stack_size)
  {
    if (t.num_args() == 1) {
      // All operators are parsed as the float type operator
      // Now that we know the actual argument type, get the version for that type
      eValueType arg_type = stack_type.last();
      Token::TokenType specialized_op = perform_type_conversion(output, t.type, arg_type);

      if (specialized_op == TokenType::NONE) {
        return false;
      }

      output.add_token(specialized_op, t.value);

      eValueType result_type = Token::result_type(specialized_op);
      stack_size -= stack_space(arg_type);
      stack_size += stack_space(result_type);
      stack_type.pop_last();
      stack_type.append(result_type);

      return true;
    }
    if (t.num_args() == 3) {
      eValueType arg1_type = stack_type.last(2);
      eValueType arg2_type = stack_type.last(1);
      eValueType arg3_type = stack_type.last(0);

      Token::TokenType specialized_op = perform_type_conversion(
          output, t.type, arg1_type, arg2_type, arg3_type);

      if (specialized_op == TokenType::NONE) {
        return false;
      }

      output.add_token(specialized_op, t.value);

      eValueType result_type = Token::result_type(specialized_op);
      stack_size -= stack_space(arg1_type);
      stack_size -= stack_space(arg2_type);
      stack_size -= stack_space(arg3_type);
      stack_size += stack_space(result_type);
      stack_type.pop_last();
      stack_type.pop_last();
      stack_type.pop_last();
      stack_type.append(result_type);

      return true;
    }

    // Assume we have a two argument operator
    // Get the arg types and a specialized op for the two types
    eValueType first_type = stack_type.last(1);
    eValueType second_type = stack_type.last(0);
    Token::TokenType specialized_op = perform_type_conversion(
        output, t.type, first_type, second_type);

    if (specialized_op == TokenType::NONE)
      return false;

    output.add_token(specialized_op, t.value);

    eValueType result_type = Token::result_type(specialized_op);
    // Assume that any conversion op doesn't change the amount of stack space used
    stack_size -= stack_space(first_type);
    stack_size -= stack_space(second_type);
    stack_size += stack_space(result_type);
    stack_type.pop_last();  // Remove the types of the arguments
    stack_type.pop_last();
    stack_type.append(result_type);  // and add that of the result

    return true;
  }

  bool push_function() {}

  bool create_postfix_program(TokenQueue const &parse_buffer,
                              TokenQueue &output,
                              std::string &error_msg)
  {
    TokenQueue operator_stack;
    Vector<Token::eValueType> stack_type;
    int stack_size = 0;  // number of float equivilents on stack

    for (int n = 0; n < parse_buffer.element_count(); n++) {
      Token t = parse_buffer.at(n);
      if (t.is_operand()) {
        if (t.is_constant())
          output_constant(t, output, stack_type, stack_size);
        else
          output_variable(t, output, stack_type, stack_size);
      }
      else if (t.is_operator_or_function()) {
        // If this operator has higher precedence than that on the stack,
        // or the stack is empty or contains a paren,
        // then push it onto the stack
        int precedence = t.precedence();
        if (operator_stack.is_empty() ||
            operator_stack.last().type == Token::TokenType::LEFT_PAREN ||
            operator_stack.last().precedence() < precedence)
        {
          operator_stack.add_token(t);
        }
        else {
          // Pop operators with higher or equal precedence off the stack and push them to output
          // then put this token on the stack
          while (!operator_stack.is_empty()) {
            Token top = operator_stack.last();
            if (top.precedence() < precedence || top.type == Token::TokenType::LEFT_PAREN)
              break;
            if (!output_op_or_function(top, output, stack_type, stack_size)) {
              error_msg = unsupported_type_error(top, stack_type);
              return false;
            }
            operator_stack.discard_last();
          };
          operator_stack.add_token(t);
        }
      }
      else if (t.type == Token::TokenType::LEFT_PAREN) {
        operator_stack.add_token(t);
      }
      else if (t.type == Token::TokenType::RIGHT_PAREN || t.type == Token::TokenType::COMMA) {
        // Pop operators off the stack until we reach the LEFT_PAREN
        while (operator_stack.last().type != Token::TokenType::LEFT_PAREN) {
          Token top = operator_stack.last();
          if (!output_op_or_function(top, output, stack_type, stack_size)) {
            error_msg = unsupported_type_error(top, stack_type);
            return false;
          }
          operator_stack.discard_last();
        };
        if (t.type == Token::TokenType::RIGHT_PAREN)
          operator_stack.discard_last();  // right paren discards the left paren
      }

      if (stack_size > MAX_STACK) {
        error_msg = std::string(TIP_("Expression uses too much stack space"));
        return false;
      }
    }

    // push any remaining operators to output
    while (!operator_stack.is_empty()) {
      Token top = operator_stack.last();
      if (!output_op_or_function(top, output, stack_type, stack_size)) {
        error_msg = unsupported_type_error(top, stack_type);
        return false;
      }
      operator_stack.discard_last();
    }

    // Push additional type conversion operations if necessary to make
    // sure the value on top of the stack is correct for the output type
    Token::eValueType top_type = stack_type.last();
    if (top_type == eValueType::INT && output_type_ != eNodeSocketDatatype::SOCK_BOOLEAN &&
        output_type_ != eNodeSocketDatatype::SOCK_INT)
    {
      // Unless we need an int type, convert to float so that code below
      // knows the top type is float or vec
      output.add_token(TokenType::CONVERT_INT_FLOAT, 0);
    }
    if (top_type == eValueType::VEC && output_type_ != eNodeSocketDatatype::SOCK_VECTOR) {
      // Need to convert a vector to a scalar type, so just take x
      output.add_token(TokenType::OPERATOR_GET_MEMBER_VEC, 2);
    }
    if (output_type_ == eNodeSocketDatatype::SOCK_VECTOR && top_type != eValueType::VEC) {
      // Just add two values to make the stack contain the vector(stack_top, 0, 0)
      output.add_token(TokenType::CONSTANT_FLOAT, 0);
      output.add_token(TokenType::CONSTANT_FLOAT, 0);
    }
    if (top_type != eValueType::INT && (output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN ||
                                        output_type_ == eNodeSocketDatatype::SOCK_INT))
    {
      output.add_token(TokenType::CONVERT_FLOAT_INT, 0);
    }

    return true;
  }

  std::string unsupported_type_error(Token t, const Vector<Token::eValueType> &stack_type)
  {
    auto token_name = std::string(token_info[(int)t.type].name) + std::string(": ");
    if (t.num_args() == 1) {
      if (stack_type.last() == eValueType::VEC)
        return token_name + TIP_(": Cannot perform this function on a vector");
    }
    else if (t.num_args() == 2) {
      auto arg1_type = stack_type.last();
      auto arg2_type = stack_type.last(1);
      if ((arg1_type == eValueType::VEC && arg2_type != eValueType::VEC) ||
          (arg1_type != eValueType::VEC && arg2_type == eValueType::VEC))
        return token_name + TIP_("Cannot mix vector and non vector types in this operation");

      if (arg1_type == eValueType::VEC && arg2_type == eValueType::VEC)
        return token_name + TIP_("Cannot perform this operation on a vector");
    }
    else if (t.num_args() == 3) {
      return token_name + TIP_("incorrect argument type");
    }

    // Catch all error message
    return token_name + TIP_(": wrong data type.");
  }

  struct RuntimeStack {
    float stack[MAX_STACK];
    int top_idx = -1;  // index of top item on stack

    inline void push_float(float val)
    {
      stack[++top_idx] = val;
    }

    inline void push_int(int val)
    {
      *(reinterpret_cast<int *>(&(stack[++top_idx]))) = val;
    }

    inline void push_vector(float3 &val)
    {
      stack[++top_idx] = val.x;
      stack[++top_idx] = val.y;
      stack[++top_idx] = val.z;
    }

    inline float pop_float()
    {
      return stack[top_idx--];
    }

    inline int pop_int()
    {
      return *reinterpret_cast<int *>(&stack[top_idx--]);
    }

    inline float3 pop_vector()
    {
      top_idx -= 3;
      return float3(stack[top_idx + 1], stack[top_idx + 2], stack[top_idx + 3]);
    }

    // Some utility methods to pop multiple args in one call, as this common
    struct PopTwoFloatsResults {
      float arg1;  // The argument pushed onto the stack first
      float arg2;
    };
    struct PopTwoIntsResults {
      int arg1;  // The argument pushed onto the stack first
      int arg2;
    };
    struct PopTwoVectorsResults {
      float3 arg1;  // The argument pushed onto the stack first
      float3 arg2;
    };

    inline PopTwoFloatsResults pop_two_floats()
    {
      top_idx -= 2;
      return PopTwoFloatsResults{stack[top_idx + 1], stack[top_idx + 2]};
    }

    inline PopTwoIntsResults pop_two_ints()
    {
      top_idx -= 2;
      int *int_stack = reinterpret_cast<int *>(stack);
      return PopTwoIntsResults{int_stack[top_idx + 1], int_stack[top_idx + 2]};
    }

    inline PopTwoVectorsResults pop_two_vectors()
    {
      top_idx -= 6;
      return PopTwoVectorsResults{
          float3(stack[top_idx + 1], stack[top_idx + 2], stack[top_idx + 3]),
          float3(stack[top_idx + 4], stack[top_idx + 5], stack[top_idx + 6])};
    }

    inline int peek_int(int offset = 0)
    {
      return *reinterpret_cast<int *>(&stack[top_idx - offset]);
    }

    inline float peek_float(int offset = 0)
    {
      return stack[top_idx - offset];
    }

    inline void replace_float(float val, int offset = 0)
    {
      stack[top_idx - offset] = val;
    }

    inline void replace_int(int val, int offset = 0)
    {
      *reinterpret_cast<int *>(&stack[top_idx - offset]) = val;
    }

    inline void discard(int amount)
    {
      top_idx -= amount;
    }
  };

  using output_variant = std::variant<float, int, bool, float3>;

  output_variant execute_program(Vector<GVArray> &inputs, int index) const
  {
    if (!program_valid_)
      return 0;

    const TokenQueue &program = program_buffer_;
    struct RuntimeStack stack;

    int num_ops = program.element_count();
    for (int i = 0; i < num_ops; i++) {
      Token t = program.at(i);
      switch (t.type) {
        case Token::TokenType::CONSTANT_FLOAT:
          stack.push_float(t.get_value_as_float());
          break;
        case Token::TokenType::CONSTANT_INT:
          stack.push_int(t.value);
          break;
        case Token::TokenType::VARIABLE_FLOAT: {
          float v = inputs[t.value].get<float>(index);
          stack.push_float(v);
        } break;
        case Token::TokenType::VARIABLE_INT: {
          int v = inputs[t.value].get<int>(index);
          stack.push_int(v);
        } break;
        case Token::TokenType::VARIABLE_BOOL: {
          bool b = inputs[t.value].get<bool>(index);
          int int_val = b ? 1 : 0;
          stack.push_int(int_val);
        } break;
        case Token::TokenType::VARIABLE_VEC: {
          float3 vv = inputs[t.value].get<float3>(index);
          stack.push_vector(vv);
        } break;
        case Token::TokenType::OPERATOR_PLUS: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = arg1 + arg2;
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_PLUS_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = arg1 + arg2;
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_PLUS_VEC: {
          auto [arg1, arg2] = stack.pop_two_vectors();
          float3 res = arg1 + arg2;
          stack.push_vector(res);
        } break;
        case Token::TokenType::OPERATOR_MINUS: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = arg1 - arg2;
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_MINUS_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = arg1 - arg2;
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_MINUS_VEC: {
          auto [arg1, arg2] = stack.pop_two_vectors();
          float3 res = arg1 - arg2;
          stack.push_vector(res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = arg1 * arg2;
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = arg1 * arg2;
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_FLOAT_VEC: {
          float3 arg2 = stack.pop_vector();
          float arg1 = stack.pop_float();
          float3 res = arg1 * arg2;
          stack.push_vector(res);
        } break;
        case Token::TokenType::OPERATOR_MULTIPLY_VEC_FLOAT: {
          float arg2 = stack.pop_float();
          float3 arg1 = stack.pop_vector();
          float3 res = arg1 * arg2;
          stack.push_vector(res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = arg2 != 0 ? arg1 / arg2 : 0;
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = arg2 != 0 ? arg1 / arg2 : 0;
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_DIVIDE_VEC_FLOAT: {
          float arg2 = stack.pop_float();
          float3 arg1 = stack.pop_vector();
          float3 res = arg1 / arg2;
          stack.push_vector(res);
        } break;
        case Token::TokenType::OPERATOR_POWER: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = pow(arg1, arg2);
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_POWER_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = pow(arg1, arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_MODULO: {
          auto [arg1, arg2] = stack.pop_two_floats();
          float res = arg2 != 0 ? fmodf(arg1, arg2) : 0;
          stack.push_float(res);
        } break;
        case Token::TokenType::OPERATOR_MODULO_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          int res = arg2 != 0 ? arg1 % arg2 : 0;
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_EQUAL: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 == arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_EQUAL_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 == arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_EQUAL_VEC: {
          auto [arg1, arg2] = stack.pop_two_vectors();
          bool res = (arg1 == arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_NOT_EQUAL: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 != arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_NOT_EQUAL_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 != arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_NOT_EQUAL_VEC: {
          auto [arg1, arg2] = stack.pop_two_vectors();
          bool res = (arg1 != arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_GREATER: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 > arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_GREATER_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 > arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_GREATER_EQUAL: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 >= arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_GREATER_EQUAL_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 >= arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_LESS: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 < arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_LESS_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 < arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_LESS_EQUAL: {
          auto [arg1, arg2] = stack.pop_two_floats();
          bool res = (arg1 <= arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_LESS_EQUAL_INT: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 <= arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_AND: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 && arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_OR: {
          auto [arg1, arg2] = stack.pop_two_ints();
          bool res = (arg1 || arg2);
          stack.push_int(res);
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS: {
          stack.push_float(-stack.pop_float());
        } break;
        case Token::TokenType::OPERATOR_UNARY_NOT: {
          stack.push_int(!stack.pop_int());
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS_INT: {
          stack.push_int(-stack.pop_int());
        } break;
        case Token::TokenType::OPERATOR_UNARY_MINUS_VEC: {
          auto vv = stack.pop_vector();
          vv = -vv;
          stack.push_vector(vv);
        } break;
        case Token::TokenType::OPERATOR_GET_MEMBER_VEC: {
          float f = stack.peek_float(t.value);
          stack.discard(3);  // discard the vector
          stack.push_float(f);
        } break;
        case Token::TokenType::FUNCTION_COSINE: {
          float res = cos(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_SINE: {
          float res = sin(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_TANGENT: {
          float res = tan(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_ASIN: {
          float res = asin(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_ACOS: {
          float res = acos(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_ATAN: {
          float res = atan(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_ATAN2: {
          auto [a, b] = stack.pop_two_floats();
          float res = atan2(a, b);
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_SQUARE_ROOT: {
          float res = sqrt(stack.pop_float());
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_MAX: {
          auto [a, b] = stack.pop_two_floats();
          float res = a > b ? a : b;
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_MAX_INT: {
          auto [a, b] = stack.pop_two_ints();
          int res = a > b ? a : b;
          stack.push_int(res);
        } break;
        case Token::TokenType::FUNCTION_MIN: {
          auto [a, b] = stack.pop_two_floats();
          float res = a < b ? a : b;
          stack.push_float(res);
        } break;
        case Token::TokenType::FUNCTION_MIN_INT: {
          auto [a, b] = stack.pop_two_ints();
          int res = a < b ? a : b;
          stack.push_int(res);
        } break;
        case Token::TokenType::FUNCTION_ABS: {
          float res = abs(stack.peek_float());
          stack.replace_float(res);
        } break;
        case Token::TokenType::FUNCTION_ABS_INT: {
          int res = abs(stack.peek_int());
          stack.replace_int(res);
        } break;
        case Token::TokenType::FUNCTION_SIGN: {
          float f = stack.peek_float();
          int res = (f > 0) - (f < 0);
          stack.replace_int(res);
        } break;
        case Token::TokenType::FUNCTION_SIGN_INT: {
          int v = stack.peek_int();
          int res = (v > 0) - (v < 0);
          stack.replace_int(res);
        } break;
        case Token::TokenType::FUNCTION_TO_RADIANS: {
          float res = stack.peek_float() * (M_PI / 180);
          stack.replace_float(res);
        } break;
        case Token::TokenType::FUNCTION_TO_DEGREES: {
          float res = stack.peek_float() * (180 / M_PI);
          stack.replace_float(res);
        } break;
        case Token::TokenType::FUNCTION_NOT: {
          stack.push_int(stack.pop_int() == 0);
        } break;
        case TokenType::FUNCTION_LOG: {
          auto [a, b] = stack.pop_two_floats();
          float res = log(a) / log(b);
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_LN: {
          float res = log(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_POW: {
          auto [a, b] = stack.pop_two_floats();
          float res = pow(a, b);
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_EXP: {
          float res = exp(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_IF: {
          float false_val = stack.pop_float();
          float true_val = stack.pop_float();
          int cond = stack.pop_int();
          float res = cond ? true_val : false_val;
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_IF_INT: {
          int false_val = stack.pop_int();
          int true_val = stack.pop_int();
          int cond = stack.pop_int();
          int res = cond ? true_val : false_val;
          stack.push_int(res);
        } break;
        case TokenType::FUNCTION_IF_VEC: {
          float3 false_val = stack.pop_vector();
          float3 true_val = stack.pop_vector();
          int cond = stack.pop_int();
          float3 res = cond ? true_val : false_val;
          stack.push_vector(res);
        } break;
        case TokenType::FUNCTION_CEIL: {
          float res = ceilf(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_FLOOR: {
          float res = floorf(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_FRAC: {
          float res = fractf(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_ROUND: {
          float res = roundf(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_TRUNCATE: {
          float res = truncf(stack.pop_float());
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_COMPARE: {
          float epsilon = stack.pop_float();
          auto [x1, x2] = stack.pop_two_floats();
          int res = compare_ff(x1, x2, epsilon);
          stack.push_int(res);
        } break;
        case TokenType::FUNCTION_COMPARE_VEC: {
          float epsilon = stack.pop_float();
          auto [v1, v2] = stack.pop_two_vectors();
          int res = compare_ff(v1.x, v2.x, epsilon) && compare_ff(v1.y, v2.y, epsilon) &&
                    compare_ff(v1.z, v2.z, epsilon);
          stack.push_int(res);
        } break;
        case TokenType::FUNCTION_DOT: {
          auto [v1, v2] = stack.pop_two_vectors();
          float res = v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
          stack.push_float(res);
        } break;
        case TokenType::FUNCTION_CROSS: {
          auto [v1, v2] = stack.pop_two_vectors();
          // Right handed co-ordinate system
          float3 res;
          res.x = v1.y * v2.z - v1.z * v2.y;
          res.y = v1.z * v2.x - v1.x * v2.z;
          res.z = v1.x * v2.y - v1.y * v2.x;
          stack.push_vector(res);
        } break;
        case TokenType::FUNCTION_NORMALIZE: {
          float3 v = stack.pop_vector();
          float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
          float len = sqrt(len2);
          float3 res = v / len;
          stack.push_vector(res);
        } break;
        case TokenType::FUNCTION_LENGTH: {
          float3 v = stack.pop_vector();
          float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
          float len = sqrt(len2);
          stack.push_float(len);
        } break;
        case TokenType::FUNCTION_LENGTH2: {
          float3 v = stack.pop_vector();
          float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
          stack.push_float(len2);
        } break;
        case Token::TokenType::CONVERT_INT_FLOAT: {
          int offset = t.value;  // may not be top of stack to convert
          int i = stack.peek_int(offset);
          float res = (float)i;
          stack.replace_float(res, offset);
        } break;
        case Token::TokenType::CONVERT_FLOAT_INT: {
          int offset = t.value;  // may not be top of stack to convert
          float f = stack.peek_float(offset);
          int res = (int)f;
          stack.replace_int(res, offset);
        } break;
        case Token::TokenType::FUNCTION_VECTOR:
          // This doesn't actually have to do anything
          // The three arguments on the stack are now the vector
          break;
        case Token::TokenType::LEFT_PAREN:
        case Token::TokenType::RIGHT_PAREN:
        case Token::TokenType::COMMA:
        case Token::TokenType::NONE:
          // These should not appear in executing programs
          break;
        default:
          BLI_assert_unreachable();
          break;
      }
    }

    // Get correct type off of stack and return it
    if (output_type_ == eNodeSocketDatatype::SOCK_FLOAT)
      return output_variant(stack.pop_float());
    else if (output_type_ == eNodeSocketDatatype::SOCK_INT)
      return output_variant(stack.pop_int());
    else if (output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN) {
      int tos = stack.pop_int();
      return output_variant(tos != 0);
    }
    else
      return output_variant(stack.pop_vector());
  }
  // Some utility methods to pop multiple args in one call, as this common
  struct PopTwoFloatsResults {
    float arg1;  // The argument pushed onto the stack first
    float arg2;
  };
  struct PopTwoIntsResults {
    int arg1;  // The argument pushed onto the stack first
    int arg2;
  };
  struct PopTwoVectorsResults {
    float3 arg1;  // The argument pushed onto the stack first
    float3 arg2;
  };

  inline PopTwoFloatsResults pop_two_floats(float stack[], int &top_idx) const
  {
    top_idx -= 2;
    return PopTwoFloatsResults{stack[top_idx + 1], stack[top_idx + 2]};
  }

  inline PopTwoIntsResults pop_two_ints(float stack[], int &top_idx) const
  {
    top_idx -= 2;
    int *int_stack = reinterpret_cast<int *>(stack);
    return PopTwoIntsResults{int_stack[top_idx + 1], int_stack[top_idx + 2]};
  }

  inline PopTwoVectorsResults pop_two_vectors(float stack[], int &top_idx) const
  {
    top_idx -= 6;
    return PopTwoVectorsResults{
        float3(stack[top_idx + 1], stack[top_idx + 2], stack[top_idx + 3]),
        float3(stack[top_idx + 4], stack[top_idx + 5], stack[top_idx + 6])};
  }
};

////////////////////////////////////////////////////////////////////////////
// ExpressionEvaluateFunction
// The multi-function used for evaluation
////////////////////////////////////////////////////////////////////////////
class ExpressionEvaluateFunction : public mf::MultiFunction {
  mf::Signature signature_;
  size_t first_output_idx_;  // Index of first output parameter
  Vector<std::string> input_identifiers_;
  Vector<short> input_types_;
  std::unique_ptr<ExpressionProgram> program_;

 public:
  ExpressionEvaluateFunction(const bNode &node, std::unique_ptr<ExpressionProgram> program)
      : program_(std::move(program))
  {
    const NodeGeometryExpression *node_storage = static_cast<NodeGeometryExpression *>(
        node.storage);

    CreateSignature(node);
    this->set_signature(&signature_);

    // Build a vector of input socket names and identifiers (excluding Expression input)
    for (int i = 1; i < node.input_sockets().size(); i++) {
      auto in_sock = node.input_sockets()[i];
      if (in_sock->typeinfo->base_cpp_type != nullptr) {
        input_identifiers_.append(in_sock->identifier);
        input_types_.append(in_sock->type);
      }
    }
  }

  // virtual ~ExpressionEvaluateFunction() {}

  void CreateSignature(const bNode &node)
  {
    mf::SignatureBuilder builder{"Expression", signature_};

    // Create the input parameters, skipping unconnected extend socket
    for (int i : node.input_sockets().index_range()) {
      if (i == 0)  // Skip Expression input
        continue;
      auto in_sock = node.input_sockets()[i];
      if (in_sock->typeinfo->base_cpp_type != nullptr)
        builder.single_input(in_sock->identifier, *in_sock->typeinfo->base_cpp_type);
    }

    // Create output params
    first_output_idx_ = signature_.params.size();
    builder.single_output("Result",
                          *bke::socket_type_to_geo_nodes_base_cpp_type(program_->output_type_));
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context context) const final
  {
    GMutableSpan results = params.uninitialized_single_output(first_output_idx_, "Result");

    // Gather the input arrays
    Vector<GVArray> input_arrays(input_identifiers_.size());
    for (int i = 0; i < input_identifiers_.size(); i++) {
      input_arrays[i] = params.readonly_single_input(i, input_identifiers_[i]);
    }

    if (program_->output_type_ == eNodeSocketDatatype::SOCK_FLOAT) {
      auto f_results = results.typed<float>();
      // Single value output type
// #define TEST
#ifdef TEST
      // Test output
      mask.foreach_index([&](const int64_t i) {
        float sum = 0;
        for (int n = 0; n < input_types_.size(); n++) {
          float val;
          switch (input_types_[n]) {
            case eNodeSocketDatatype::SOCK_FLOAT:
              val = input_arrays[n].get<float>(i);
              break;
            case eNodeSocketDatatype::SOCK_INT:
              val = input_arrays[n].get<int32_t>(i);
              break;
            case eNodeSocketDatatype::SOCK_BOOLEAN:
              val = input_arrays[n].get<bool>(i) ? 1.0f : 0.0f;
              break;
            case eNodeSocketDatatype::SOCK_VECTOR:
              val = input_arrays[n].get<float3>(i).x;
              break;
            default:
              // Unsupported type
              BLI_assert_unreachable();
              val = 0;
              break;
          }
          sum += val;
        }
        results[i] = sum;
      });
#else
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        f_results[i] = std::get<float>(val);
      });
#endif
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_INT) {
      auto i_results = results.typed<int>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        i_results[i] = std::get<int>(val);
      });
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_BOOLEAN) {
      auto i_results = results.typed<bool>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        i_results[i] = std::get<bool>(val);
      });
    }
    else if (program_->output_type_ == eNodeSocketDatatype::SOCK_VECTOR) {
      // Vector output type
      auto v_results = results.typed<float3>();
      mask.foreach_index([&](const int64_t i) {
        auto val = program_->execute_program(input_arrays, i);
        float3 res = std::get<float3>(val);
        v_results[i] = res;
      });
    }
  }

  ExecutionHints get_execution_hints() const final
  {
    ExecutionHints hints;
    hints.min_grain_size = 512;
    return hints;
  }
};

////////////////////////////////////////////////////////////////////////////
// Node Functions
// The standard set of funtions for the node
////////////////////////////////////////////////////////////////////////////

NODE_STORAGE_FUNCS(NodeGeometryExpression);

static bool is_supported_socket_type(const eNodeSocketDatatype data_type)
{
  return ELEM(data_type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  // Bizarrely these two lines are necessary to set b.is_context_dependent
  const bNodeTree *ntree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node == nullptr)
    return;

  b.add_input<decl::String>("Expression")
      .default_value(std::string("x"))
      .compact(true);  // hide_label();

  // Add the variable number of input sockets
  const NodeGeometryExpression &storage = node_storage(*node);
  for (const NodeExpressionItem &eq_item : storage.socket_items.items()) {
    const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(eq_item);
    const eNodeSocketDatatype dataType = (eNodeSocketDatatype)eq_item.socket_type;
    auto &input = b.add_input(dataType, eq_item.name, identifier)
                      .socket_name_ptr(
                          &ntree->id, ExpressionItemsAccessor::item_srna, &eq_item, "name");
    input.supports_field();
  }

  // Add extension socket
  b.add_input<decl::Extend>("", "__extend__");

  // Add outputs
  const eNodeSocketDatatype output_type = eNodeSocketDatatype(storage.output_type);
  auto output = b.add_output(output_type, "Result");
  output.dependent_field();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryExpression *data = MEM_cnew<NodeGeometryExpression>(__func__);

  data->socket_items.next_identifier = 0;
  data->socket_items.items_array = nullptr;
  data->socket_items.items_num = 0;
  data->output_type = eNodeSocketDatatype::SOCK_FLOAT;

  node->storage = data;

  // Add a couple of predefined inputs
  data->socket_items.items_array = MEM_cnew_array<NodeExpressionItem>(2, __func__);
  ExpressionItemsAccessor::init_with_socket_type_and_name(
      *node, data->socket_items.items_array[0], eNodeSocketDatatype::SOCK_FLOAT, "x");
  //  ExpressionItemsAccessor::init_with_socket_type_and_name(
  //      *node, data->socket_items.items_array[1], eNodeSocketDatatype::SOCK_FLOAT, "y");
  data->socket_items.items_num = 1;
}

static void node_free_storage(bNode *node)
{
  NodeGeometryExpression *data = reinterpret_cast<NodeGeometryExpression *>(node->storage);
  if (!data)
    return;

  socket_items::destruct_array<ExpressionItemsAccessor>(*node);
  MEM_freeN(node->storage);
  node->storage = nullptr;  // free_storage seems to get called twice at shutdown, so protect
                            // against double free
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryExpression &src_storage = node_storage(*src_node);
  NodeGeometryExpression *dst_storage = (NodeGeometryExpression *)MEM_cnew<NodeGeometryExpression>(
      __func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ExpressionItemsAccessor>(*src_node, *dst_node);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  auto storage = reinterpret_cast<NodeGeometryExpression *>(node->storage);
  int starting_sockets_num = storage->socket_items.items_num;

  bool ok = socket_items::try_add_item_via_any_extend_socket<ExpressionItemsAccessor>(
      *ntree, *node, *node, *link);

  // If the link wasn't added or it's an output, we're done
  if (!ok || link->fromnode == node)
    return ok;

  // If it's the expression socket, allow connection from string socket only
  if (STREQ(link->tosock->identifier, "Expression"))
    return link->fromsock->type == eNodeSocketDatatype::SOCK_STRING;

  // If we didn't add a new socket, then an existing one got reused. Check the type is valid as
  // try_add_item_via_any_extend_socket doesn't check this
  if (starting_sockets_num == storage->socket_items.items_num) {
    if (!ExpressionItemsAccessor::supports_socket_type(
            (eNodeSocketDatatype)(link->fromsock->type)))
      return false;
  }

  // Find the index of the added link
  // int item_index = storage->socket_items.items_num - 1;  // Newly added item is always last
  int item_index = -1;
  for (int i = 0; i < storage->socket_items.items_num; i++) {
    if (strcmp(link->tosock->name, storage->socket_items.items_array[i].name) == 0) {
      item_index = i;
      break;
    }
  }
  if (item_index == -1)
    return ok;  // shouldn't happen

  // Update the socket type
  storage->socket_items.items_array[item_index].socket_type = link->fromsock->type;

  // If we didn't add a new socket then no need to rename
  if (starting_sockets_num == storage->socket_items.items_num)
    return ok;

  // If we're connecting to a socket that's renamable, then keep the existing name
  const bNode *f_node = link->fromnode;
  if (f_node->is_group_input() || f_node->is_group_output() ||
      f_node->is_type("GeometryNodeRepeatInput") || f_node->is_type("GeometryNodeRepeatOutput") ||
      f_node->is_type("GeometryNodeForeachGeometryElementInput") ||
      f_node->is_type("GeometryNodeForeachGeometryElementOutput"))
  {
    // Replace any spaces in the name with underscores
    const char *item_name = storage->socket_items.items_array[item_index].name;
    bool has_space = false;
    for (int i = 0; i < strlen(item_name); i++)
      has_space |= std::isspace(item_name[i]);
    if (has_space) {
      auto new_name = BLI_strdup(item_name);
      for (int i = 0; i < strlen(item_name); i++)
        if (std::isspace(item_name[i]))
          new_name[i] = '_';
      MEM_SAFE_FREE(storage->socket_items.items_array[item_index].name);
      storage->socket_items.items_array[item_index].name = new_name;
    }
    return ok;
  }

  // If the item has a single char name it's probably ok, so don't change it
  const char *item_name = storage->socket_items.items_array[item_index].name;
  if (strlen(item_name) == 1)
    return ok;

  // rename the new connect to something more convienient than the default
  const char *new_name = nullptr;
  bool free_new_name = false;
  if (item_index == 0)
    new_name = "x";
  else {
    auto prev_name = storage->socket_items.items_array[item_index - 1].name;
    new_name = ExpressionItemsAccessor::get_new_unique_name(*node, prev_name);
    free_new_name = true;
  }
  if (new_name) {
    MEM_SAFE_FREE(storage->socket_items.items_array[item_index].name);
    storage->socket_items.items_array[item_index].name = BLI_strdupn(new_name, strlen(new_name));
    if (free_new_name)
      MEM_SAFE_FREE(new_name);
  }

  return ok;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "output_type", UI_ITEM_NONE, "", ICON_NONE);
  // uiItemR(layout, ptr, "axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
  // uiLayoutSetPropSep(layout, true);
  // uiLayoutSetPropDecorate(layout, false);
  // uiItemR(layout, ptr, "pivot_axis", UI_ITEM_NONE, IFACE_("Pivot"), ICON_NONE);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);

  uiItemR(layout, ptr, "output_type", UI_ITEM_NONE, "", ICON_NONE);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "Expression_items", false, IFACE_("Variables"))) {
    socket_items::ui::draw_items_list_with_operators<ExpressionItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<ExpressionItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          uiLayoutSetPropSep(panel, true);
          uiLayoutSetPropDecorate(panel, false);
          uiItemR(panel, item_ptr, "description", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  if (!params.output_is_required("Result"))
    return;

  // Get the Expression
  std::string Expression = params.get_input<std::string>("Expression");

  // If no Expression, do nothing
  if (Expression.size() == 0) {
    // params.error_message_add(NodeWarningType::Error, TIP_("Expression"));
    params.set_default_remaining_outputs();
    return;
  }

  // Get the output type
  const bNode &node = params.node();
  const NodeGeometryExpression &storage = *(const NodeGeometryExpression *)(node.storage);
  uint8_t oType = storage.output_type;

  // Build vectors of input names and types (excluding Expression socket, and extend socket)
  Vector<const char *> input_names;
  Vector<short> input_types;
  for (int i = 1; i < node.input_sockets().size(); i++) {
    auto in_sock = node.input_sockets()[i];
    if (in_sock->typeinfo->base_cpp_type != nullptr) {
      input_names.append(in_sock->name);
      input_types.append(in_sock->type);
    }
  }

  // Create a program from the expression
  std::string error_msg;
  std::unique_ptr<ExpressionProgram> program = std::make_unique<ExpressionProgram>(
      &input_names, &input_types, (eNodeSocketDatatype)oType);
  if (!program->create_program(Expression, error_msg)) {
    params.error_message_add(NodeWarningType::Error, error_msg);
    params.set_default_remaining_outputs();
    return;
  }

  // Build vectors of input fields, excluding initial name field
  // and final extend field
  Vector<GField> input_fields;
  for (int i = 1; i < node.input_sockets().size(); i++) {
    auto in_sock = node.input_sockets()[i];
    if (in_sock->typeinfo->base_cpp_type != nullptr) {
      GField f = params.extract_input<GField>(in_sock->identifier);
      input_fields.append(std::move(f));
    }
  }

  // Create a FieldOperation with a multi-function to do the actual evaluation
  auto mf = std::make_unique<ExpressionEvaluateFunction>(params.node(), std::move(program));
  GField f_calculated_results{FieldOperation::Create(std::move(mf), input_fields)};

  // And set the output to the FieldOperation
  params.set_output<GField>("Result", std::move(f_calculated_results));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "output_type",
      "Output Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_storage_enum_accessors(output_type),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return is_supported_socket_type(eNodeSocketDatatype(item.value));
            });
      });
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (data_type == SOCK_STRING) {
      params.add_item(IFACE_("Expression"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeExpression");
        params.update_and_connect_available_socket(node, "Expression");
      });
    }
  }
  else {
    if (is_supported_socket_type(data_type)) {
      params.add_item(IFACE_("Results"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeExpression");
        node_storage(node).output_type = params.socket.type;
        params.update_and_connect_available_socket(node, "Result");
      });
    }
  }
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ExpressionItemsAccessor>();
}

static void node_register()
{
#ifndef NDEBUG
  token_info_check();
#endif

  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeExpression", std::nullopt);
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate a string as a mathmatical Expression";
  ntype.nclass = NODE_CLASS_CONVERTER;

  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.register_operators = node_operators;

  // blender::bke::node_register_type(&ntype);
  blender::bke::node_type_storage(
      ntype, "NodeGeometryExpression", node_free_storage, node_copy_storage);

  // stash this auto assigmed value
  ExpressionItemsAccessor::node_type = ntype.type_legacy;

  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);

  //  node_geo_Expression_cc::node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc

////////////////////////////////////////////////////
// blender::nodes namespace
////////////////////////////////////////////////////
namespace blender::nodes {

StructRNA *ExpressionItemsAccessor::item_srna = &RNA_NodeExpressionItem;
int ExpressionItemsAccessor::node_type = 0;  // needs to be set during node registration
int ExpressionItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(NodeExpressionItem);

void ExpressionItemsAccessor::blend_write_item(BlendWriter *writer, const NodeExpressionItem &item)
{
  BLO_write_string(writer, item.name);
  BLO_write_string(writer, item.description);
}

void ExpressionItemsAccessor::blend_read_data_item(BlendDataReader *reader,
                                                   NodeExpressionItem &item)
{
  BLO_read_string(reader, &item.name);
  BLO_read_string(reader, &item.description);
}

}  // namespace blender::nodes

blender::Span<NodeExpressionItem> NodeExpressionItems::items() const
{
  return blender::Span<NodeExpressionItem>(items_array, items_num);
}

blender::MutableSpan<NodeExpressionItem> NodeExpressionItems::items()
{
  return blender::MutableSpan<NodeExpressionItem>(items_array, items_num);
}
