
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <jvmti.h>

static jvmtiEnv *global_jvmti = NULL;
static bool global_jvm_started = false;
static const std::vector<std::string> global_java_prefixes = {
  "Ljava/",
  "Ljdk/",
  "Lsun/"
};

struct java_value
{
  enum java_type
  {
    INT,
    LONG,
    DOUBLE,
    FLOAT,
    OBJECT
  };
  union _value
  {
    jobject _object;
    jint _int;
    jlong _long;
    jdouble _double;
    jfloat _float;
  };

  java_type type;
  _value value;
};

typedef std::unordered_map<std::string, java_value> state_map;

struct single_step
{
  std::string class_name;
  std::string method_name;
  state_map class_state;
  state_map instance_state;
  state_map local_state;
};

static std::vector<single_step> global_program_steps;

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

void write_toml()
{
  std::ostringstream output;

  for (int i = 0; i < global_program_steps.size(); ++i) {
    const single_step& step = global_program_steps[i];
    output
      << "["
      << "step" << i << "."
      << "\"" << step.class_name << "\"."
      << "\"" << step.method_name << "\""
      << "]"
      << std::endl;

    for (const auto& var : step.local_state) {
      output
        << "local."
        << "\"" << var.first << "\" = ";
      switch (var.second.type) {
      case java_value::INT: output << var.second.value._int; break;
      case java_value::LONG: output << var.second.value._long; break;
      case java_value::DOUBLE: output << var.second.value._double; break;
      case java_value::FLOAT: output << var.second.value._float; break;
      default: output << var.second.value._object;
      }
      output << std::endl;
    }
  }

  if (std::getenv("JTRACE_OUT") != NULL) {
    std::string filename(std::getenv("JTRACE_OUT"));
    std::ofstream file;
    file.open(filename);
    file << output.str();
    file.close();
  } else std::cout << output.str();
}

java_value get_local_variable(
  jvmtiEnv *jvmti, jthread thread, int depth, int slot, std::string signature
  )
{
  jvmtiError error;
  java_value value;
  java_value::java_type type;

#define GET_LOCAL_VARIABLE(method, member) {                            \
    error = (method)(thread, depth, slot, &(value.value.member));       \
    if (error != JVMTI_ERROR_INVALID_SLOT)                              \
      check_jvmti_error(jvmti, error, "unable to get local variable");  \
    value.type = type;                                                  \
  }

  // yuck
  if (signature == "I") {
    type = java_value::INT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalInt, _int);
  } else if (signature == "L") {
    type = java_value::LONG;
    GET_LOCAL_VARIABLE(jvmti->GetLocalLong, _long);
  } else if (signature == "F") {
    type = java_value::FLOAT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalFloat, _float);
  } else if (signature == "D") {
    type = java_value::DOUBLE;
    GET_LOCAL_VARIABLE(jvmti->GetLocalDouble, _double);
  } else {
    type = java_value::OBJECT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalObject, _object);
  }

  return value;
}

void JNICALL cb_single_step(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  jthread thread,
  jmethodID method,
  jlocation location
  )
{
  if (!global_jvm_started) return;

  static std::unordered_map<jmethodID, std::string> method_classes;
  jvmtiError error;

  if (method_classes.count(method) == 0) {
    jclass klass;
    error = jvmti->GetMethodDeclaringClass(method, &klass);
    check_jvmti_error(jvmti, error, "unable to get class");

    char *_class_signature = NULL;
    char *_class_generic = NULL;
    error = jvmti->GetClassSignature(klass, &_class_signature, &_class_generic);
    check_jvmti_error(jvmti, error, "unable to get signature");

    method_classes[method] = std::string(_class_signature);
    std::string class_signature(_class_signature);
  }

  std::string class_signature(method_classes[method]);
  for (const auto& prefix : global_java_prefixes) {
    const auto cmp_substr = class_signature.substr(0, prefix.size());
    if (cmp_substr == prefix) return;
  }

  single_step current_step;
  current_step.class_name = class_signature;

  char *_method_name = NULL;
  char *_method_signature = NULL;
  char *_method_generic = NULL;
  error = jvmti->GetMethodName(
    method, &_method_name, &_method_signature, &_method_generic
    );
  check_jvmti_error(jvmti, error, "unable to get method name");
  if (_method_name != NULL) current_step.method_name = _method_name;

  jvmtiLocalVariableEntry *_var_table = NULL;
  jint _var_entry_count;
  error = jvmti->GetLocalVariableTable(method, &_var_entry_count, &_var_table);
  check_jvmti_error(jvmti, error, "unable to get local variable table");

  std::vector<jvmtiLocalVariableEntry> local_vars(
    _var_table, _var_table + _var_entry_count
    );
  for (const auto& var : local_vars) {
    std::string var_signature(var.signature);
    java_value var_value = get_local_variable(
      jvmti, thread, 0, var.slot, var_signature
      );
    current_step.local_state[std::string(var.name)] = var_value;
  }

  global_program_steps.push_back(current_step);
}

void JNICALL cb_vm_start(jvmtiEnv *jvmti, JNIEnv *jni)
{
  std::cerr << "VM started" << std::endl;
  global_jvm_started = true;
}

void JNICALL cb_vm_death(jvmtiEnv *jvmti, JNIEnv *jni)
{
  std::cerr << "VM died" << std::endl;
  write_toml();
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

  callbacks.SingleStep = cb_single_step;
  callbacks.VMStart = cb_vm_start;
  callbacks.VMDeath = cb_vm_death;

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
  error = global_jvmti->SetEventNotificationMode(
    JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, (jthread) NULL
    );
  check_jvmti_error(global_jvmti, error, "unable to set event notification");

  return JNI_OK;
}
