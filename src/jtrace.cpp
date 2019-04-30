
// jtrace.cpp
//
// A native agent that traces Java code execution.
//
// author: Ajay Tatachar <ajaymt2@illinois.edu>

// Useful documentation:
// https://docs.oracle.com/javase/8/docs/platform/jvmti/jvmti.html
// https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/functions.html

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <jvmti.h>
#include <jni.h>

// For now we don't trace code inside the Java stdlib, there will eventually be
// a better way to ignore specific classes/packages.
static const std::vector<std::string> global_java_prefixes = {
  "Ljava/",
  "Ljdk/",
  "Lsun/"
};
// Receiver class name
static const std::string global_jtrace_receiver = "JTraceReceiver;";
// Receiver method signature
static const std::string global_receive_signature = "(Ljava/lang/String;I)V";

// Every Java value has a type and a signature. The value is stored as a
// union of all underlying JVMTI types.
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
  std::string signature;
  java_value() : type(INT) { std::memset(&value, 0, sizeof(value)); }
};

// State is captured as a map of identifiers to values.
typedef std::unordered_map<std::string, java_value> state_map;

// Every execution step is inside a method and has local, instance and class
// state.
struct single_step
{
  std::string class_name;
  std::string method_name;
  state_map class_state;
  state_map instance_state;
  state_map local_state;
};

// Putting all the global variables in a single struct makes them seem
// less bad.
struct global_state
{
  bool jvm_started = false;
  bool state_only = false;
  jmethodID receiver_method = 0;
  std::vector<single_step> program_steps;
};
static global_state global;

// We overload this operator to be able to tell whether state changed between
// two steps.
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
      if (var.second.signature != right_val.signature)          \
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

// Exceptionally rudimentary error handling.
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
  jvmti->Deallocate((unsigned char *)errnum_str);
}

// Serialize a single state_map into TOML.
void write_state(std::ostream& output, std::string prefix, const state_map& map)
{
  for (const auto& var : map) {
    output
      << prefix << "."
      << "\"" << var.first << "\".signature = "
      << "\"" << var.second.signature << "\""
      << std::endl
      << prefix << "."
      << "\"" << var.first << "\".value = ";
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

// Send the tracing information to the receiver.
void send_steps(JNIEnv *jni, jclass receiver)
{
  if (receiver == 0 || global.receiver_method == 0)
    return;

  std::ostringstream output;

  for (int i = 0; i < global.program_steps.size(); ++i) {
    const single_step& step = global.program_steps[i];
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

  std::string outstr = output.str();
  jchar output_chars[outstr.size()];
  for (int i = 0; i < outstr.size(); ++i)
    output_chars[i] = (jchar)outstr[i];
  jstring output_string = jni->NewString(output_chars, (jsize)outstr.size());
  jni->CallStaticVoidMethod(
    receiver,
    global.receiver_method,
    output_string,
    (jint)global.program_steps.size()
    );
}

// Read the value of a local variable.
// Every variable exists in a specific `slot` (something like an offset)
// within a stack frame.
// `depth` is the depth of the stack frame -- `0` reads from the current
// stack frame, `1` reads from the previous frame, etc.
bool get_local_variable(
  jvmtiEnv *jvmti, jthread thread, int depth, int slot, java_value& value
  )
{
  jvmtiError error;
  java_value::java_type type;

#define GET_LOCAL_VARIABLE(method, member) {                            \
    error = (method)(thread, depth, slot, &(value.value.member));       \
    if (error == JVMTI_ERROR_INVALID_SLOT)                              \
      return false;                                                     \
    check_jvmti_error(jvmti, error, "unable to get local variable");    \
    value.type = type;                                                  \
  }

  // yuck
  if (value.signature == "I") {
    type = java_value::INT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalInt, _int);
  } else if (value.signature == "J") {
    type = java_value::LONG;
    GET_LOCAL_VARIABLE(jvmti->GetLocalLong, _long);
  } else if (value.signature == "F") {
    type = java_value::FLOAT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalFloat, _float);
  } else if (value.signature == "D") {
    type = java_value::DOUBLE;
    GET_LOCAL_VARIABLE(jvmti->GetLocalDouble, _double);
  } else {
    type = java_value::OBJECT;
    GET_LOCAL_VARIABLE(jvmti->GetLocalObject, _object);
  }

  return true;
}

// Read a field from an object or class.
void read_field(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  single_step& step,
  jclass klass,
  jfieldID field
  )
{
  // We know we're in an instance if `this` is bound locally.
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
  jvmti->Deallocate((unsigned char *)_field_name);
  jvmti->Deallocate((unsigned char *)_field_signature);
  jvmti->Deallocate((unsigned char *)_field_generic);

  jint _field_modifiers = 0;
  error = jvmti->GetFieldModifiers(klass, field, &_field_modifiers);
  check_jvmti_error(jvmti, error, "unable to get field modifiers");

#define FIELD_STATIC_MODIFIER 0x0008

  // Don't do anything if it's an instance field and we're in a static context.
  bool is_static = _field_modifiers & FIELD_STATIC_MODIFIER;
  if (!is_static && !is_instance) return;

  java_value field_value;
  java_value::java_type type;
  jobject _obj = is_instance ? step.local_state["this"].value._object : 0;
  field_value.signature = field_signature;

#define READ_FIELD(s_method, i_method, member) {                        \
    if (is_static) field_value.value.member = (s_method)(klass, field); \
    else field_value.value.member = (i_method)(_obj, field);            \
    field_value.type = type;                                            \
  }

  // yuck
  // see: http://ftp.magicsoftware.com/www/help/mg9/How/howto_java_vm_type_signatures.htm
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

// Cache class and method names in the global scope because these are used by
// more than one function.
static std::unordered_map<jmethodID, std::string> method_classes;
static std::unordered_map<jmethodID, std::string> method_names;

// Single step callback that records state while tracing.
void JNICALL cb_single_step(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  jthread thread,
  jmethodID method,
  jlocation location
  )
{
  // Cache field and local variable tables to not be slow.
  static std::unordered_map<jmethodID, bool> method_traceable;
  static std::unordered_map<jmethodID, std::vector<jfieldID>> method_fields;
  static std::unordered_map<
    jmethodID,
    std::vector<jvmtiLocalVariableEntry>
    > method_local_variables;

  if (!global.jvm_started) return;

  if (method_traceable.count(method) > 0 && !method_traceable[method])
    return;

  jvmtiError error;
  jclass klass;
  error = jvmti->GetMethodDeclaringClass(method, &klass);
  check_jvmti_error(jvmti, error, "unable to get class");

  // Cache class name.
  if (method_classes.count(method) == 0) {
    char *_class_signature = NULL;
    char *_class_generic = NULL;
    error = jvmti->GetClassSignature(klass, &_class_signature, &_class_generic);
    check_jvmti_error(jvmti, error, "unable to get signature");
    method_classes[method] = _class_signature;
    jvmti->Deallocate((unsigned char *)_class_signature);
    jvmti->Deallocate((unsigned char *)_class_generic);
  }

  // Ignore classes within the Java stdlib.
  std::string class_signature(method_classes[method]);
  for (const auto& prefix : global_java_prefixes) {
    const auto cmp_substr = class_signature.substr(0, prefix.size());
    if (cmp_substr == prefix) {
      method_traceable[method] = false;
      return;
    }
  }

  // Ignore the receiver class.
  if (class_signature.size() >= global_jtrace_receiver.size()) {
    std::string suffix = class_signature.substr(
      class_signature.size() - global_jtrace_receiver.size()
      );
    if (suffix == global_jtrace_receiver) {
      method_traceable[method] = false;
      return;
    }
  }

  method_traceable[method] = true;

  // Cache method name.
  // TODO: include method signature because name does not uniquely
  // identify methods.
  if (method_names.count(method) == 0) {
    char *_method_name = NULL;
    char *_method_signature = NULL;
    char *_method_generic = NULL;
    error = jvmti->GetMethodName(
      method, &_method_name, &_method_signature, &_method_generic
      );
    check_jvmti_error(jvmti, error, "unable to get method name");
    method_names[method] = _method_name;
    jvmti->Deallocate((unsigned char *)_method_name);
    jvmti->Deallocate((unsigned char *)_method_signature);
    jvmti->Deallocate((unsigned char *)_method_generic);
  }

  // Cache local variable table.
  if (method_local_variables.count(method) == 0) {
    jvmtiLocalVariableEntry *_var_table = NULL;
    jint _var_entry_count;
    error = jvmti->GetLocalVariableTable(
      method, &_var_entry_count, &_var_table
      );
    check_jvmti_error(jvmti, error, "unable to get local variable table");
    method_local_variables[method].assign(
      _var_table, _var_table + _var_entry_count
      );
    jvmti->Deallocate((unsigned char *)_var_table);
  }

  single_step current_step;
  current_step.class_name = class_signature;
  current_step.method_name = method_names[method];

  // Read all local variables.
  for (const auto& var : method_local_variables[method]) {
    java_value var_value;
    var_value.signature = var.signature;
    bool exists = get_local_variable(
      jvmti, thread, 0, var.slot, var_value
      );
    if (exists)
      current_step.local_state[std::string(var.name)] = var_value;
  }

  // `this` does not behave like other local variables
  if (current_step.local_state.count("this")) {
    jobject _this = 0;
    error = jvmti->GetLocalInstance(thread, 0, &_this);
    check_jvmti_error(jvmti, error, "unable to get local instance");
    current_step.local_state["this"].value._object = _this;
  }

  // Cache field table.
  if (method_fields.count(method) == 0) {
    jfieldID *_field_table = NULL;
    jint _field_entry_count;
    error = jvmti->GetClassFields(klass, &_field_entry_count, &_field_table);
    check_jvmti_error(jvmti, error, "unable to get class field");
    method_fields[method].assign(
      _field_table, _field_table + _field_entry_count
      );
    jvmti->Deallocate((unsigned char *)_field_table);
  }

  // Read all fields.
  for (const auto& field : method_fields[method])
    read_field(jvmti, jni, current_step, klass, field);

  // If the receiver wants only state changes, we exclude steps that
  // don't change state.
  if (
    global.state_only
    && global.program_steps.size() > 0
    && global.program_steps.back() == current_step
    ) return;
  global.program_steps.push_back(current_step);
}

// Method entry callback that starts and stops the tracing.
void JNICALL cb_method_enter(
  jvmtiEnv *jvmti,
  JNIEnv *jni,
  jthread thread,
  jmethodID method
  )
{
  if (!global.jvm_started) return;

  jvmtiError error;
  jclass klass;
  error = jvmti->GetMethodDeclaringClass(method, &klass);
  check_jvmti_error(jvmti, error, "unable to get class");

  // Cache class name.
  if (method_classes.count(method) == 0) {
    char *_class_signature = NULL;
    char *_class_generic = NULL;
    error = jvmti->GetClassSignature(klass, &_class_signature, &_class_generic);
    check_jvmti_error(jvmti, error, "unable to get signature");
    method_classes[method] = _class_signature;
    jvmti->Deallocate((unsigned char *)_class_signature);
    jvmti->Deallocate((unsigned char *)_class_generic);
  }

  // Ignore classes that aren't the receiver.
  std::string class_signature(method_classes[method]);
  if (class_signature.size() < global_jtrace_receiver.size()) return;
  if (
    class_signature.substr(
      class_signature.size() - global_jtrace_receiver.size()
      ) != global_jtrace_receiver
    ) return;

  // Cache method name.
  if (method_names.count(method) == 0) {
    char *_method_name = NULL;
    char *_method_signature = NULL;
    char *_method_generic = NULL;
    error = jvmti->GetMethodName(
      method, &_method_name, &_method_signature, &_method_generic
      );
    check_jvmti_error(jvmti, error, "unable to get method name");
    method_names[method] = _method_name;
    jvmti->Deallocate((unsigned char *)_method_name);
    jvmti->Deallocate((unsigned char *)_method_signature);
    jvmti->Deallocate((unsigned char *)_method_generic);
  }

  if (method_names[method] == "start") { // Start tracing.
    if (global.receiver_method == 0) {
      jmethodID receive_method = jni->GetStaticMethodID(
        klass, "receive", global_receive_signature.data()
        );
      global.receiver_method = receive_method;
    }

    // Cache the "filterSteps" field.
    static jfieldID state_only_field = 0;
    if (state_only_field == 0)
      state_only_field = jni->GetStaticFieldID(klass, "stateOnly", "Z");

    // Check if the receiver wants to filter recorded steps.
    if (state_only_field)
      global.state_only = (bool)jni->GetStaticBooleanField(
        klass, state_only_field
        );

    // Enable VM single-step notifications.
    error = jvmti->SetEventNotificationMode(
      JVMTI_ENABLE, JVMTI_EVENT_SINGLE_STEP, (jthread) NULL
      );
    check_jvmti_error(jvmti, error, "unable to set event notification");
  } else if (method_names[method] == "end") { // Stop tracing.
    // Disable VM single-step notifications.
    error = jvmti->SetEventNotificationMode(
      JVMTI_DISABLE, JVMTI_EVENT_SINGLE_STEP, (jthread) NULL
      );
    check_jvmti_error(jvmti, error, "unable to set event notification");

    // Send trace info to the receiver.
    send_steps(jni, klass);

    // clear program steps
    global.program_steps.clear();
  }

  return;
}

// VM start callback.
void JNICALL cb_vm_start(jvmtiEnv *jvmti, JNIEnv *jni)
{
  global.jvm_started = true;
}

// 'OnLoad' callback that initializes everything.
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved)
{
  jvmtiEnv *jvmti = NULL;

  jint res = jvm->GetEnv((void **)&jvmti, JVMTI_VERSION_1_0);
  if (res != JNI_OK || jvmti == NULL) {
    std::cerr << "unable to access JVMTI version 1.0" << std::endl;
    return JNI_ERR;
  }

  jvmtiCapabilities capa;
  jvmtiError error;
  std::memset(&capa, 0, sizeof(capa));

  capa.can_generate_single_step_events = 1;
  capa.can_generate_method_entry_events = 1;
  capa.can_get_line_numbers = 1;
  capa.can_get_source_file_name = 1;
  capa.can_access_local_variables = 1;

  error = jvmti->AddCapabilities(&capa);
  check_jvmti_error(
    jvmti, error, "unable to set necessary capabilities"
    );

  jvmtiEventCallbacks callbacks;
  std::memset(&callbacks, 0, sizeof(callbacks));

  callbacks.SingleStep = cb_single_step;
  callbacks.VMStart = cb_vm_start;
  callbacks.MethodEntry = cb_method_enter;

  error = jvmti->SetEventCallbacks(&callbacks, (jint) sizeof(callbacks));
  check_jvmti_error(jvmti, error, "unable to set event callbacks");

  error = jvmti->SetEventNotificationMode(
    JVMTI_ENABLE, JVMTI_EVENT_METHOD_ENTRY, (jthread) NULL
    );
  check_jvmti_error(jvmti, error, "unable to set event notification");
  error = jvmti->SetEventNotificationMode(
    JVMTI_ENABLE, JVMTI_EVENT_VM_START, (jthread) NULL
    );
  check_jvmti_error(jvmti, error, "unable to set event notification");

  return JNI_OK;
}
