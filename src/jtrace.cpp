
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
#include <jni.h>

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
    SHORT,
    CHAR,
    BYTE,
    BOOLEAN,
    OBJECT
  };
  union _value
  {
    jobject _object;
    jint _int;
    jshort _short;
    jchar _char;
    jboolean _boolean;
    jbyte _byte;
    jlong _long;
    jdouble _double;
    jfloat _float;
  };

  java_type type;
  _value value;
  java_value () : type(INT) { std::memset(&value, 0, sizeof(value)); }
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

bool operator==(const single_step& left, const single_step& right)
{
  if (left.class_name != right.class_name) return false;
  if (left.method_name != right.method_name) return false;
  if (left.class_state.size() != right.class_state.size())
    return false;
  if (left.instance_state.size() != right.instance_state.size())
    return false;
  if (left.local_state.size() != right.local_state.size())
    return false;

#define COMPARE_MAP(map) {                                      \
    for (const auto& var : left.map) {                          \
      if (right.map.count(var.first) == 0)                      \
        return false;                                           \
      const java_value& right_val = right.map.at(var.first);    \
      if (var.second.type != right_val.type)                    \
        return false;                                           \
      if (var.second.value._object != right_val.value._object)  \
        return false;                                           \
    }                                                           \
  }

  COMPARE_MAP(class_state);
  COMPARE_MAP(instance_state);
  COMPARE_MAP(local_state);

  return true;
}

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

void write_state(std::ostringstream& output, std::string prefix, state_map map)
{
  for (const auto& var : map) {
    output
      << prefix << "."
      << "\"" << var.first << "\" = ";
    switch (var.second.type) {
    case java_value::INT: output << var.second.value._int; break;
    case java_value::SHORT: output << var.second.value._short; break;
    case java_value::LONG: output << var.second.value._long; break;
    case java_value::DOUBLE: output << var.second.value._double; break;
    case java_value::FLOAT: output << var.second.value._float; break;
    case java_value::BOOLEAN: output << var.second.value._int; break; //
    case java_value::BYTE: output << var.second.value._byte; break;
    case java_value::CHAR: output << var.second.value._char; break;
    default: output << var.second.value._object;
    }
    output << std::endl;
  }
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

    write_state(output, "local", step.local_state);
    write_state(output, "instance", step.instance_state);
    write_state(output, "class", step.class_state);
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
  } else if (signature == "J") {
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

void read_field(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  single_step& step,
  jclass klass,
  jfieldID field
  )
{
  bool is_instance = step.local_state.count("this");
  jvmtiError error;

  char *_field_name = NULL;
  char *_field_signature = NULL;
  char *_field_generic = NULL;
  error = jvmti->GetFieldName(
    klass, field, &_field_name, &_field_signature, &_field_generic
    );
  check_jvmti_error(jvmti, error, "unable to get class field");
  std::string field_name(_field_name);
  std::string field_signature(_field_signature);

  jint _field_modifiers = 0;
  error = jvmti->GetFieldModifiers(klass, field, &_field_modifiers);
  check_jvmti_error(jvmti, error, "unable to get field modifiers");

#define FIELD_STATIC_MODIFIER 0x0008

  bool is_static = _field_modifiers & FIELD_STATIC_MODIFIER;
  if (!is_static && !is_instance) return;

  java_value field_value;
  java_value::java_type type;
  jobject _obj = is_instance ? step.local_state["this"].value._object : 0;

#define READ_FIELD(s_method, i_method, member) {                        \
    if (is_static) field_value.value.member = (s_method)(klass, field); \
    else field_value.value.member = (i_method)(_obj, field);            \
    field_value.type = type;                                            \
  }

  // yuck
  if (field_signature == "I") {
    type = java_value::INT;
    READ_FIELD(jni->GetStaticIntField, jni->GetIntField, _int);
  } else if (field_signature == "J") {
    type = java_value::LONG;
    READ_FIELD(jni->GetStaticLongField, jni->GetLongField, _long);
  } else if (field_signature == "F") {
    type = java_value::FLOAT;
    READ_FIELD(jni->GetStaticFloatField, jni->GetFloatField, _float);
  } else if (field_signature == "D") {
    type = java_value::DOUBLE;
    READ_FIELD(jni->GetStaticDoubleField, jni->GetDoubleField, _double);
  } else if (field_signature == "Z") {
    type = java_value::BOOLEAN;
    READ_FIELD(jni->GetStaticBooleanField, jni->GetBooleanField, _boolean);
  } else if (field_signature == "B") {
    type = java_value::BYTE;
    READ_FIELD(jni->GetStaticByteField, jni->GetByteField, _byte);
  } else if (field_signature == "C") {
    type = java_value::CHAR;
    READ_FIELD(jni->GetStaticCharField, jni->GetCharField, _char);
  } else if (field_signature == "S") {
    type = java_value::SHORT;
    READ_FIELD(jni->GetStaticShortField, jni->GetShortField, _short);
  } else {
    type = java_value::OBJECT;
    READ_FIELD(jni->GetStaticObjectField, jni->GetObjectField, _object);
  }

  if (is_instance) step.instance_state[field_name] = field_value;
  else step.class_state[field_name] = field_value;
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
  static std::unordered_map<jmethodID, std::string> method_names;
  static std::unordered_map<
    jmethodID,
    std::vector<jvmtiLocalVariableEntry>
    > method_local_variables;
  jvmtiError error;

  jclass klass;
  error = jvmti->GetMethodDeclaringClass(method, &klass);
  check_jvmti_error(jvmti, error, "unable to get class");

  if (method_classes.count(method) == 0) {
    char *_class_signature = NULL;
    char *_class_generic = NULL;
    error = jvmti->GetClassSignature(klass, &_class_signature, &_class_generic);
    check_jvmti_error(jvmti, error, "unable to get signature");
    method_classes[method] = _class_signature;
  }

  std::string class_signature(method_classes[method]);
  for (const auto& prefix : global_java_prefixes) {
    const auto cmp_substr = class_signature.substr(0, prefix.size());
    if (cmp_substr == prefix) return;
  }

  if (method_names.count(method) == 0) {
    char *_method_name = NULL;
    char *_method_signature = NULL;
    char *_method_generic = NULL;
    error = jvmti->GetMethodName(
      method, &_method_name, &_method_signature, &_method_generic
      );
    check_jvmti_error(jvmti, error, "unable to get method name");
    method_names[method] = _method_name;
  }

  if (method_local_variables.count(method) == 0) {
    jvmtiLocalVariableEntry *_var_table = NULL;
    jint _var_entry_count;
    error = jvmti->GetLocalVariableTable(method, &_var_entry_count, &_var_table);
    check_jvmti_error(jvmti, error, "unable to get local variable table");
    method_local_variables[method].assign(
      _var_table, _var_table + _var_entry_count
      );
  }

  single_step current_step;
  current_step.class_name = class_signature;
  current_step.method_name = method_names[method];

  for (const auto& var : method_local_variables[method]) {
    std::string var_signature(var.signature);
    java_value var_value = get_local_variable(
      jvmti, thread, 0, var.slot, var_signature
      );
    current_step.local_state[std::string(var.name)] = var_value;
  }

  if (current_step.local_state.count("this")) {
    // 'this' does not behave like other local variables
    jobject _this = 0;
    error = jvmti->GetLocalInstance(thread, 0, &_this);
    check_jvmti_error(jvmti, error, "unable to get local instance");
    current_step.local_state["this"].value._object = _this;
  }

  jfieldID *_field_table = NULL;
  jint _field_entry_count;
  error = jvmti->GetClassFields(klass, &_field_entry_count, &_field_table);
  check_jvmti_error(jvmti, error, "unable to get class field");

  std::vector<jfieldID> class_fields(
    _field_table, _field_table + _field_entry_count
    );
  for (const auto& field : class_fields)
    read_field(jvmti, jni, current_step, klass, field);

  if (
    global_program_steps.size() > 0
    && global_program_steps.back() == current_step
    ) return;
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
