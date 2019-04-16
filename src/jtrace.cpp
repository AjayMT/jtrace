
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <jvmti.h>

static jvmtiEnv *global_jvmti = NULL;
static int global_jvm_started = 0;
static const std::vector<std::string> global_java_prefixes = {
  "Ljava/",
  "Ljdk/",
  "Lsun/"
};

void check_jvmti_error(
  jvmtiEnv *jvmti,
  jvmtiError errnum,
  const char *str
  )
{
  if (errnum == JVMTI_ERROR_NONE) return;

  char *errnum_str = NULL;
  jvmti->GetErrorName(errnum, &errnum_str);
  std::cerr
    << "ERROR: JVMTI: "
    << errnum << "("
    << (errnum_str == NULL ? "Unknown" : errnum_str) << "): "
    << (str == NULL ? "" : str)
    << std::endl;
}

void JNICALL cbSingleStep(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  jthread thread,
  jmethodID method,
  jlocation location
  )
{
  if (!global_jvm_started) return;

  static std::unordered_map<jmethodID, bool> method_classes;
  jvmtiError error;

  if (method_classes.count(method) == 0) {
    jclass klass;
    error = jvmti->GetMethodDeclaringClass(method, &klass);
    check_jvmti_error(jvmti, error, "unable to get class");

    char *_class_signature = NULL;
    char *_class_generic = NULL;
    error = jvmti->GetClassSignature(klass, &_class_signature, &_class_generic);
    check_jvmti_error(jvmti, error, "unable to get signature");

    method_classes[method] = true;
    std::string class_signature(_class_signature);
    for (const auto& prefix : global_java_prefixes) {
      const auto cmp_substr = class_signature.substr(0, prefix.size());
      if (cmp_substr == prefix) {
        method_classes[method] = false;
        break;
      }
    }
  }

  if (!method_classes[method]) return;

  char *_method_name = NULL;
  char *_method_signature = NULL;
  char *_method_generic = NULL;
  error = jvmti->GetMethodName(method, &_method_name, &_method_signature, &_method_generic);
  check_jvmti_error(jvmti, error, "unable to get method name");

  jvmtiLocalVariableEntry *_var_table = NULL;
  jint _var_entry_count;
  error = jvmti->GetLocalVariableTable(method, &_var_entry_count, &_var_table);
  check_jvmti_error(jvmti, error, "unable to get local variable table");

  std::string method_name(_method_name);
  std::vector<jvmtiLocalVariableEntry> local_vars(_var_table, _var_table + _var_entry_count);

  std::cout << "method name: " << method_name << std::endl;
  for (const auto& var : local_vars) {
    std::cout << "  local variable: " << var.name << std::endl;
  }
}

void JNICALL cbVMStart(jvmtiEnv *jvmti, JNIEnv *jni)
{
  std::cout << "VM started" << std::endl;
  global_jvm_started = 1;
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved)
{
  jint res = jvm->GetEnv((void **)&global_jvmti, JVMTI_VERSION_1_0);
  if (res != JNI_OK || global_jvmti == NULL) {
    std::cerr << "unable to access JVMTI version 1.0" << std::endl;
    return JNI_ERR;
  }

  jvmtiCapabilities capa;
  jvmtiError error;
  std::memset(&capa, 0, sizeof(capa));

  capa.can_generate_single_step_events = 1;
  capa.can_get_line_numbers = 1;
  capa.can_get_source_file_name = 1;
  capa.can_access_local_variables = 1;

  error = global_jvmti->AddCapabilities(&capa);
  check_jvmti_error(global_jvmti, error, "unable to set necessary capabilities");

  jvmtiEventCallbacks callbacks;
  std::memset(&callbacks, 0, sizeof(callbacks));

  callbacks.SingleStep = cbSingleStep;
  callbacks.VMStart = cbVMStart;

  error = global_jvmti->SetEventCallbacks(&callbacks, (jint) sizeof(callbacks));
  check_jvmti_error(global_jvmti, error, "unable to set event callbacks");

  error = global_jvmti->SetEventNotificationMode(
    JVMTI_ENABLE, JVMTI_EVENT_SINGLE_STEP, (jthread) NULL
    );
  check_jvmti_error(global_jvmti, error, "unable to set event notification");
  error = global_jvmti->SetEventNotificationMode(
    JVMTI_ENABLE, JVMTI_EVENT_VM_START, (jthread) NULL
    );
  check_jvmti_error(global_jvmti, error, "unable to set event notification");

  return JNI_OK;
}
