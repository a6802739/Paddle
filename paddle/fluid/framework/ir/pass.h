/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <functional>
#include <map>
#include <string>

#include "paddle/fluid/framework/ir/graph.h"
#include "paddle/fluid/framework/ir/node.h"
#include "paddle/fluid/framework/program_desc.h"
#include "paddle/fluid/platform/port.h"
#include "paddle/fluid/platform/variant.h"

namespace paddle {
namespace framework {
namespace ir {
template <typename PassType>
struct PassRegistrar;

class Pass {
 public:
  Pass() = default;
  virtual ~Pass() {
    for (auto &attr : attrs_) {
      if (attr_dels_.find(attr.first) != attr_dels_.end()) {
        attr_dels_[attr.first]();
      }
    }
    attrs_.clear();
    attr_dels_.clear();
  }

  std::unique_ptr<Graph> Apply(std::unique_ptr<Graph> graph) const;

  // Get a reference to the attributed previously set.
  template <typename AttrType>
  AttrType &Get(const std::string &attr_name) const {
    PADDLE_ENFORCE(attrs_.find(attr_name) != attrs_.end(),
                   "%s attr not registered for pass.", attr_name);
    return *boost::any_cast<AttrType *>(attrs_.at(attr_name));
  }

  // Set a pointer to the attribute. Pass takes ownership of the attribute.
  template <typename AttrType>
  void Set(const std::string &attr_name, AttrType *attr) {
    PADDLE_ENFORCE(attrs_.count(attr_name) == 0, "%s already set in the pass",
                   attr_name);
    attrs_[attr_name] = attr;
    attr_dels_[attr_name] = [attr, attr_name]() {
      VLOG(3) << "deleting " << attr_name;
      delete attr;
    };
  }

  // Set a pointer to the attribute. Pass doesn't take ownership. Caller
  // should delete the attribute.
  template <typename AttrType>
  void SetNotOwned(const std::string &attr_name, AttrType *attr) {
    PADDLE_ENFORCE(attrs_.count(attr_name) == 0);
    attrs_[attr_name] = attr;
  }

 protected:
  virtual std::unique_ptr<Graph> ApplyImpl(
      std::unique_ptr<Graph> graph) const = 0;

 private:
  template <typename PassType>
  friend struct PassRegistrar;

  void RegisterRequiredPassAttrs(const std::unordered_set<std::string> &attrs) {
    required_pass_attrs_.insert(attrs.begin(), attrs.end());
  }

  void RegisterRequiredGraphAttrs(
      const std::unordered_set<std::string> &attrs) {
    required_graph_attrs_.insert(attrs.begin(), attrs.end());
  }

  mutable bool applied_{false};
  std::unordered_set<std::string> required_pass_attrs_;
  std::unordered_set<std::string> required_graph_attrs_;
  std::map<std::string, boost::any> attrs_;
  std::map<std::string, std::function<void(void)>> attr_dels_;
};

using PassCreator = std::function<std::unique_ptr<Pass>()>;

class Registrar {
 public:
  // In our design, various kinds of passes,
  // have their corresponding registry and registrar. The action of
  // registration is in the constructor of a global registrar variable, which
  // are not used in the code that calls package framework, and would
  // be removed from the generated binary file by the linker. To avoid such
  // removal, we add Touch to all registrar classes and make USE_PASS macros to
  // call this method. So, as long as the callee code calls USE_PASS, the global
  // registrar variable won't be removed by the linker.
  void Touch() {}
};

class PassRegistry {
 public:
  static PassRegistry &Instance();

  bool Has(const std::string &pass_type) const {
    return map_.find(pass_type) != map_.end();
  }

  void Insert(const std::string &pass_type, const PassCreator &pass_creator) {
    PADDLE_ENFORCE(!Has(pass_type), "Pass %s has been registered", pass_type);
    map_.insert({pass_type, pass_creator});
  }

  std::unique_ptr<Pass> Get(const std::string &pass_type) const {
    PADDLE_ENFORCE(Has(pass_type), "Pass %s has not been registered",
                   pass_type);
    return map_.at(pass_type)();
  }

 private:
  PassRegistry() = default;
  std::unordered_map<std::string, PassCreator> map_;

  DISABLE_COPY_AND_ASSIGN(PassRegistry);
};

template <typename PassType>
struct PassRegistrar : public Registrar {
  explicit PassRegistrar(const char *pass_type) {
    PADDLE_ENFORCE(!PassRegistry::Instance().Has(pass_type),
                   "'%s' is registered more than once.", pass_type);
    PassRegistry::Instance().Insert(
        pass_type, [this]() -> std::unique_ptr<Pass> {
          std::unique_ptr<Pass> pass(new PassType());
          pass->RegisterRequiredPassAttrs(this->required_pass_attrs_);
          pass->RegisterRequiredGraphAttrs(this->required_graph_attrs_);
          return pass;
        });
  }

  PassRegistrar<PassType> &RequirePassAttr(const std::string &attr) {
    required_pass_attrs_.insert(attr);
    return *this;
  }

  PassRegistrar<PassType> &RequireGraphAttr(const std::string &attr) {
    required_graph_attrs_.insert(attr);
    return *this;
  }

 private:
  std::unordered_set<std::string> required_pass_attrs_;
  std::unordered_set<std::string> required_graph_attrs_;
};

#define STATIC_ASSERT_PASS_GLOBAL_NAMESPACE(uniq_name, msg)                   \
  struct __test_global_namespace_##uniq_name##__ {};                          \
  static_assert(std::is_same<::__test_global_namespace_##uniq_name##__,       \
                             __test_global_namespace_##uniq_name##__>::value, \
                msg)

#if !defined(_WIN32)
// Register a new pass that can be applied on the IR.
#define REGISTER_PASS(pass_type, pass_class)                          \
  STATIC_ASSERT_PASS_GLOBAL_NAMESPACE(                                \
      __reg_pass__##pass_type,                                        \
      "REGISTER_PASS must be called in global namespace");            \
  static ::paddle::framework::ir::PassRegistrar<pass_class>           \
      __pass_registrar_##pass_type##__(#pass_type);                   \
  int TouchPassRegistrar_##pass_type() {                              \
    __pass_registrar_##pass_type##__.Touch();                         \
    return 0;                                                         \
  }                                                                   \
  static ::paddle::framework::ir::PassRegistrar<pass_class>           \
      &__pass_tmp_registrar_##pass_type##__ __attribute__((unused)) = \
          __pass_registrar_##pass_type##__

#define USE_PASS(pass_type)                                           \
  STATIC_ASSERT_PASS_GLOBAL_NAMESPACE(                                \
      __use_pass_itself_##pass_type,                                  \
      "USE_PASS must be called in global namespace");                 \
  extern int TouchPassRegistrar_##pass_type();                        \
  static int use_pass_itself_##pass_type##_ __attribute__((unused)) = \
      TouchPassRegistrar_##pass_type()
#else
// windows version of __attribute__((unused))
#define UNUSED(x) __pragma(warning(suppress : 4100)) x
#define REGISTER_PASS(pass_type, pass_class)                        \
  STATIC_ASSERT_PASS_GLOBAL_NAMESPACE(                              \
      __reg_pass__##pass_type,                                      \
      "REGISTER_PASS must be called in global namespace");          \
  static ::paddle::framework::ir::PassRegistrar<pass_class>         \
      __pass_registrar_##pass_type##__(#pass_type);                 \
  int TouchPassRegistrar_##pass_type() {                            \
    __pass_registrar_##pass_type##__.Touch();                       \
    return 0;                                                       \
  }                                                                 \
  static ::paddle::framework::ir::PassRegistrar<pass_class> UNUSED( \
      &__pass_tmp_registrar_##pass_type##__) =                      \
      __pass_registrar_##pass_type##__

#define USE_PASS(pass_type)                           \
  STATIC_ASSERT_PASS_GLOBAL_NAMESPACE(                \
      __use_pass_itself_##pass_type,                  \
      "USE_PASS must be called in global namespace"); \
  extern int TouchPassRegistrar_##pass_type();        \
  static int UNUSED(use_pass_itself_##pass_type##_) = \
      TouchPassRegistrar_##pass_type()

#endif  // !_WIN32
}  // namespace ir
}  // namespace framework
}  // namespace paddle
