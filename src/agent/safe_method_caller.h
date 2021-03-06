/**
 * Copyright 2015 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DEVTOOLS_CDBG_DEBUGLETS_JAVA_SAFE_METHOD_CALLER_H_
#define DEVTOOLS_CDBG_DEBUGLETS_JAVA_SAFE_METHOD_CALLER_H_

#include "class_file.h"
#include "class_files_cache.h"
#include "class_indexer.h"
#include "config.h"
#include "class_metadata_reader.h"
#include "common.h"
#include "jni_utils.h"
#include "jobject_map.h"
#include "jvariant.h"
#include "messages.h"
#include "method_caller.h"
#include "method_call_result.h"
#include "model.h"
#include "nanojava_interpreter.h"

namespace devtools {
namespace cdbg {

// Invokes methods either through JNI or with built-in NanoJava interpreter.
// This class is not thread safe. It should be instantiated for a single
// method call or for series of calls within the same expression.
class SafeMethodCaller
    : public MethodCaller,
      public nanojava::NanoJavaInterpreter::Supervisor {
 public:
  // "config" and "class_indexer" are not owned by this class and must outlive
  // it. The configuration has a separate quota for expressions and pretty
  // printers, hence passing it explicitly, rather than getting from "config".
  SafeMethodCaller(
      const Config* config,
      Config::MethodCallQuota quota,
      ClassIndexer* class_indexer,
      ClassFilesCache* class_files_cache);

  ~SafeMethodCaller() override;

  // Gets the total number of instructions processed by the interpreter.
  int total_instructions_counter() const {
    return total_instructions_counter_;
  }

  // Gets the name of the currently interpreted method. Returns empty string
  // if no method is being executed. This is used to print the current method
  // name in error messages generated by "NanoJavaInterpreter::Supervisor"
  // callbacks.
  string GetCurrentMethodName() const;

  //
  // Implementation of "MethodCaller" interface.
  //

  // "Invoke" is used to call Java method from an expression or pretty printer.
  ErrorOr<JVariant> Invoke(
      const ClassMetadataReader::Method& metadata,
      const JVariant& source,
      std::vector<JVariant> arguments) override;

  // Common code for the outer and nested method invocation. Also used by
  // safe caller proxies.
  MethodCallResult InvokeInternal(
      bool nonvirtual,
      const ClassMetadataReader::Method& metadata,
      jobject source,
      std::vector<JVariant> arguments);

  //
  // Implementation of "NanoJavaInterpreter::Supervisor" interface.
  //

  // "InvokeInternal" is used by safe caller internally to support method calls
  // from within other methods.
  MethodCallResult InvokeNested(
      bool nonvirtual,
      const ConstantPool::MethodRef& method,
      jobject source,
      std::vector<JVariant> arguments) override;

  std::unique_ptr<FormatMessageModel> IsNextInstructionAllowed() override;

  void NewObjectAllocated(jobject obj) override;

  std::unique_ptr<FormatMessageModel> IsNewArrayAllowed(int32 count) override;

  std::unique_ptr<FormatMessageModel> IsArrayModifyAllowed(
      jobject array) override;

  std::unique_ptr<FormatMessageModel> IsFieldModifyAllowed(
      jobject target,
      const ConstantPool::FieldRef& field) override;

 private:
  // Classes that play a role when calling a method.
  struct CallTarget {
    // Class that implemented the method to be executed.
    JniLocalRef method_cls;

    // Signature of "method_cls".
    string method_cls_signature;

    // The class returned by Java statement "obj.getClass()".
    JniLocalRef object_cls;

    // Signature of "object_cls".
    string object_cls_signature;

    // Policy of the method.
    const Config::Method* method_config;
  };

  // Checks if the interpreter is effectively disabled.
  bool IsNanoJavaInterpreterDisabled() const {
    return (quota_.max_classes_load == 0) &&
           (quota_.max_interpreter_instructions == 0);
  }

  // Gets the classes of the invoked method.
  ErrorOr<CallTarget> GetCallTarget(
      bool nonvirtual,
      const ClassMetadataReader::Method& metadata,
      jobject source);

  // Format call stack of the interpreted methods.
  string CurrentCallStack() const;

  // Checks if the specified object was created during expression evaluation
  // (and therefore is not part of application state).
  bool IsTemporaryObject(jobject obj) const;

  // Verifies that arguments match the expected signature. Returns success
  // result if everything is good to go.
  MethodCallResult CheckArguments(
      const JMethodSignature& signature,
      const std::vector<JVariant>& arguments);

  // Verifies that the value stored in "JVariant" matches the signature.
  bool CheckSignature(
      const JSignature& signature,
      const JVariant& value) const;

  // Lazily loads class file if quota allows.
  ErrorOr<std::unique_ptr<ClassFilesCache::AutoClassFile>> CacheLoadClassFile(
      jobject cls);

  // Formats "Method not safe to call" error message.
  MethodCallResult MethodBlocked(
      const ClassMetadataReader::Method& metadata,
      const CallTarget& call_target) const;

  // Calls the target method with JNI.
  MethodCallResult InvokeJni(
      bool nonvirtual,
      const ClassMetadataReader::Method& metadata,
      jobject source,
      std::vector<JVariant> arguments,
      const CallTarget& call_target);

  // Calls the target method with NanoJava interpreter.
  MethodCallResult InvokeInterpreter(
      const ClassMetadataReader::Method& metadata,
      jobject source,
      std::vector<JVariant> arguments,
      const CallTarget& call_target);

 private:
  // We want to use JobjectMap as a set, so we map key to empty structure.
  struct Empty { };

  // Policy for method calls.
  const Config* const config_;

  // Quota settings for method calls invoked by this instance
  // of "SafeMethodCaller".
  const Config::MethodCallQuota quota_;

  // Resolves class signature to java.lang.Class<?> objects.
  ClassIndexer* const class_indexer_;

  // Global cache of loaded class files for safe caller.
  ClassFilesCache* const class_files_cache_;

  // Currently interpreted method. The interpreter keeps a reference to its
  // parent. This way we can reconstruct the interpreter call stack for
  // debugging purposes.
  const nanojava::NanoJavaInterpreter* current_interpreter_ = nullptr;

  // Total number of instructions processed by the interpreter.
  // Does not count JNI calls.
  int total_instructions_counter_ = 0;

  // Counts number of methods that were loaded as part of method execution.
  // Methods fetched from cache are not counted.
  int total_method_load_counter_ = 0;

  // Set of temporary objects created during expression evaluation. We do not
  // consider these objects as part of application state. Therefore we allow
  // methods invoked from expressions to change instance fields of such objects.
  JobjectMap<JObject_GlobalRef, Empty> temporary_objects_;

  DISALLOW_COPY_AND_ASSIGN(SafeMethodCaller);
};

}  // namespace cdbg
}  // namespace devtools

#endif  // DEVTOOLS_CDBG_DEBUGLETS_JAVA_SAFE_METHOD_CALLER_H_
