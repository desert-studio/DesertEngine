#ifndef RFL_JSON_WRITER_HPP_
#define RFL_JSON_WRITER_HPP_

#if __has_include(<yyjson.h>)
#include <yyjson.h>
#else
#include "../thirdparty/yyjson.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../Result.hpp"
#include "../always_false.hpp"

namespace rfl {
namespace json {

class Writer {
 public:
  struct YYJSONOutputArray {
    YYJSONOutputArray(yyjson_mut_val* _val) : val_(_val) {}
    yyjson_mut_val* val_;
  };

  struct YYJSONOutputObject {
    YYJSONOutputObject(yyjson_mut_val* _val) : val_(_val) {}
    yyjson_mut_val* val_;
  };

  struct YYJSONOutputVar {
    YYJSONOutputVar(yyjson_mut_val* _val) : val_(_val) {}

    YYJSONOutputVar(YYJSONOutputArray _arr) : val_(_arr.val_) {}

    YYJSONOutputVar(YYJSONOutputObject _obj) : val_(_obj.val_) {}

    yyjson_mut_val* val_;
  };

  using OutputArrayType = YYJSONOutputArray;
  using OutputObjectType = YYJSONOutputObject;
  using OutputVarType = YYJSONOutputVar;

  Writer(yyjson_mut_doc* _doc);

  OutputArrayType array_as_root(const size_t) const noexcept;

  OutputObjectType object_as_root(const size_t) const noexcept;

  OutputVarType null_as_root() const noexcept;

  template <class T>
  OutputVarType value_as_root(const T& _var) const noexcept {
    const auto val = from_basic_type(_var);
    yyjson_mut_doc_set_root(doc_, val.val_);
    return OutputVarType(val);
  }

  OutputArrayType add_array_to_array(const size_t,
                                     OutputArrayType* _parent) const noexcept;

  OutputArrayType add_array_to_object(const std::string_view& _name,
                                      const size_t,
                                      OutputObjectType* _parent) const noexcept;

  OutputObjectType add_object_to_array(const size_t,
                                       OutputArrayType* _parent) const noexcept;

  OutputObjectType add_object_to_object(
      const std::string_view& _name, const size_t,
      OutputObjectType* _parent) const noexcept;

  template <class T>
  OutputVarType add_value_to_array(const T& _var,
                                   OutputArrayType* _parent) const noexcept {
    const auto val = from_basic_type(_var);
    yyjson_mut_arr_add_val(_parent->val_, val.val_);
    return OutputVarType(val);
  }

  template <class T>
  OutputVarType add_value_to_object(const std::string_view& _name,
                                    const T& _var,
                                    OutputObjectType* _parent) const noexcept {
    const auto val = from_basic_type(_var);
    yyjson_mut_obj_add(_parent->val_, yyjson_mut_strcpy(doc_, _name.data()),
                       val.val_);
    return OutputVarType(val);
  }

  OutputVarType add_null_to_array(OutputArrayType* _parent) const noexcept;

  OutputVarType add_null_to_object(const std::string_view& _name,
                                   OutputObjectType* _parent) const noexcept;

  void end_array(OutputArrayType*) const noexcept;

  void end_object(OutputObjectType*) const noexcept;

 private:
  static double shortest_float_as_double(const float _var) noexcept {
    const double widened = static_cast<double>(_var);
    if (!std::isfinite(_var)) {
      return widened;
    }
    char text[32];
    for (int digits = 1; digits <= 9; ++digits) {
      std::snprintf(text, sizeof(text), "%.*g", digits, widened);
      const double candidate = std::strtod(text, nullptr);
      if (static_cast<float>(candidate) == _var) {
        return candidate;
      }
    }
    return widened;
  }

  template <class T>
  OutputVarType from_basic_type(const T& _var) const noexcept {
    if constexpr (std::is_same<std::remove_cvref_t<T>, std::string>()) {
      return OutputVarType(yyjson_mut_strcpy(doc_, _var.c_str()));
    } else if constexpr (std::is_same<std::remove_cvref_t<T>, bool>()) {
      return OutputVarType(yyjson_mut_bool(doc_, _var));
    } else if constexpr (std::is_same<std::remove_cvref_t<T>, float>()) {
      // DESERT: a float is written as the SHORTEST decimal that reads back as the same float ("0.9", not
      // the double 0.8999999761581421 the widened value would spell). yyjson 0.10 has no float writer, so
      // the shortest float decimal is found here and handed over as the double nearest to it, whose own
      // shortest spelling is that decimal. Guarded: if the double does not narrow back to the same float,
      // the exact widened value is written instead — the value is never changed, only its spelling.
      return OutputVarType(yyjson_mut_real(doc_, shortest_float_as_double(_var)));
    } else if constexpr (std::is_floating_point<std::remove_cvref_t<T>>()) {
      return OutputVarType(yyjson_mut_real(doc_, static_cast<double>(_var)));
    } else if constexpr (std::is_unsigned<std::remove_cvref_t<T>>()) {
      return OutputVarType(yyjson_mut_uint(doc_, static_cast<uint64_t>(_var)));
    } else if constexpr (std::is_integral<std::remove_cvref_t<T>>()) {
      return OutputVarType(yyjson_mut_int(doc_, static_cast<int64_t>(_var)));
    } else {
      static_assert(rfl::always_false_v<T>, "Unsupported type.");
    }
  }

 public:
  yyjson_mut_doc* doc_;
};

}  // namespace json
}  // namespace rfl

#endif
