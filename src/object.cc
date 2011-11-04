// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"

#include <string.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>

#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "globals.h"
#include "heap.h"
#include "intern_table.h"
#include "logging.h"
#include "monitor.h"
#include "runtime.h"
#include "stack.h"
#include "utils.h"

namespace art {

Object* Object::Clone() {
  Class* c = GetClass();
  DCHECK(!c->IsClassClass());

  // Object::SizeOf gets the right size even if we're an array.
  // Using c->AllocObject() here would be wrong.
  size_t num_bytes = SizeOf();
  SirtRef<Object> copy(Heap::AllocObject(c, num_bytes));
  if (copy.get() == NULL) {
    return NULL;
  }

  // Copy instance data.  We assume memcpy copies by words.
  // TODO: expose and use move32.
  byte* src_bytes = reinterpret_cast<byte*>(this);
  byte* dst_bytes = reinterpret_cast<byte*>(copy.get());
  size_t offset = sizeof(Object);
  memcpy(dst_bytes + offset, src_bytes + offset, num_bytes - offset);

  if (c->IsFinalizable()) {
    Heap::AddFinalizerReference(Thread::Current(), copy.get());
  }

  return copy.get();
}

uint32_t Object::GetThinLockId() {
  return Monitor::GetThinLockId(monitor_);
}

bool Object::IsString() const {
  // TODO use "klass_ == String::GetJavaLangString()" instead?
  return GetClass() == GetClass()->GetDescriptor()->GetClass();
}

void Object::MonitorEnter(Thread* thread) {
  Monitor::MonitorEnter(thread, this);
}

bool Object::MonitorExit(Thread* thread) {
  return Monitor::MonitorExit(thread, this);
}

void Object::Notify() {
  Monitor::Notify(Thread::Current(), this);
}

void Object::NotifyAll() {
  Monitor::NotifyAll(Thread::Current(), this);
}

void Object::Wait(int64_t ms, int32_t ns) {
  Monitor::Wait(Thread::Current(), this, ms, ns, true);
}

// TODO: get global references for these
Class* Field::java_lang_reflect_Field_ = NULL;

void Field::SetClass(Class* java_lang_reflect_Field) {
  CHECK(java_lang_reflect_Field_ == NULL);
  CHECK(java_lang_reflect_Field != NULL);
  java_lang_reflect_Field_ = java_lang_reflect_Field;
}

void Field::ResetClass() {
  CHECK(java_lang_reflect_Field_ != NULL);
  java_lang_reflect_Field_ = NULL;
}

void Field::SetTypeIdx(uint32_t type_idx) {
  SetField32(OFFSET_OF_OBJECT_MEMBER(Field, type_idx_), type_idx, false);
}

Class* Field::GetTypeDuringLinking() const {
  // We are assured that the necessary primitive types are in the dex cache
  // early during class linking
  return GetDeclaringClass()->GetDexCache()->GetResolvedType(GetTypeIdx());
}

bool Field::IsPrimitiveType() const {
  Class* type = GetTypeDuringLinking();
  return (type == NULL || type->IsPrimitive());
}

Primitive::Type Field::GetPrimitiveType() const {
  Class* type = GetTypeDuringLinking();
  if (type == NULL) {
    return Primitive::kPrimNot;
  }
  return type->GetPrimitiveType();
}

size_t Field::PrimitiveSize() const {
  return Primitive::FieldSize(GetPrimitiveType());
}

const char* Field::GetTypeDescriptor() const {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(GetDeclaringClass()->GetDexCache());
  const char* descriptor = dex_file.StringByTypeIdx(GetTypeIdx());
  DCHECK(descriptor != NULL);
  return descriptor;
}

Class* Field::GetType() {
  Class* type = GetFieldObject<Class*>(OFFSET_OF_OBJECT_MEMBER(Field, type_), false);
  if (type == NULL) {
    type = Runtime::Current()->GetClassLinker()->ResolveType(GetTypeIdx(), this);
    SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Field, type_), type, false);
  }
  return type;
}

void Field::SetOffset(MemberOffset num_bytes) {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  Primitive::Type type = GetPrimitiveType();
  if (type == Primitive::kPrimDouble || type == Primitive::kPrimLong) {
    DCHECK_ALIGNED(num_bytes.Uint32Value(), 8);
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Field, offset_), num_bytes.Uint32Value(), false);
}

void Field::InitJavaFields() {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  MonitorEnter(self);
  if (type_ == NULL) {
    InitJavaFieldsLocked();
  }
  MonitorExit(self);
}

void Field::InitJavaFieldsLocked() {
  GetType(); // Resolves type as a side-effect. May throw.
}

uint32_t Field::Get32(const Object* object) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  return object->GetField32(GetOffset(), IsVolatile());
}

void Field::Set32(Object* object, uint32_t new_value) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  object->SetField32(GetOffset(), new_value, IsVolatile());
}

uint64_t Field::Get64(const Object* object) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  return object->GetField64(GetOffset(), IsVolatile());
}

void Field::Set64(Object* object, uint64_t new_value) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  object->SetField64(GetOffset(), new_value, IsVolatile());
}

Object* Field::GetObj(const Object* object) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  return object->GetFieldObject<Object*>(GetOffset(), IsVolatile());
}

void Field::SetObj(Object* object, const Object* new_value) const {
  CHECK((object == NULL) == IsStatic()) << PrettyField(this);
  if (IsStatic()) {
    object = declaring_class_;
  }
  object->SetFieldObject(GetOffset(), new_value, IsVolatile());
}

bool Field::GetBoolean(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimBoolean) << PrettyField(this);
  return Get32(object);
}

void Field::SetBoolean(Object* object, bool z) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimBoolean) << PrettyField(this);
  Set32(object, z);
}

int8_t Field::GetByte(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimByte) << PrettyField(this);
  return Get32(object);
}

void Field::SetByte(Object* object, int8_t b) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimByte) << PrettyField(this);
  Set32(object, b);
}

uint16_t Field::GetChar(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimChar) << PrettyField(this);
  return Get32(object);
}

void Field::SetChar(Object* object, uint16_t c) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimChar) << PrettyField(this);
  Set32(object, c);
}

int16_t Field::GetShort(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimShort) << PrettyField(this);
  return Get32(object);
}

void Field::SetShort(Object* object, int16_t s) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimShort) << PrettyField(this);
  Set32(object, s);
}

int32_t Field::GetInt(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimInt) << PrettyField(this);
  return Get32(object);
}

void Field::SetInt(Object* object, int32_t i) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimInt) << PrettyField(this);
  Set32(object, i);
}

int64_t Field::GetLong(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimLong) << PrettyField(this);
  return Get64(object);
}

void Field::SetLong(Object* object, int64_t j) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimLong) << PrettyField(this);
  Set64(object, j);
}

float Field::GetFloat(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimFloat) << PrettyField(this);
  JValue float_bits;
  float_bits.i = Get32(object);
  return float_bits.f;
}

void Field::SetFloat(Object* object, float f) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimFloat) << PrettyField(this);
  JValue float_bits;
  float_bits.f = f;
  Set32(object, float_bits.i);
}

double Field::GetDouble(const Object* object) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimDouble) << PrettyField(this);
  JValue double_bits;
  double_bits.j = Get64(object);
  return double_bits.d;
}

void Field::SetDouble(Object* object, double d) const {
  DCHECK(GetPrimitiveType() == Primitive::kPrimDouble) << PrettyField(this);
  JValue double_bits;
  double_bits.d = d;
  Set64(object, double_bits.j);
}

Object* Field::GetObject(const Object* object) const {
  CHECK(GetPrimitiveType() == Primitive::kPrimNot) << PrettyField(this);
  return GetObj(object);
}

void Field::SetObject(Object* object, const Object* l) const {
  CHECK(GetPrimitiveType() == Primitive::kPrimNot) << PrettyField(this);
  SetObj(object, l);
}

bool Method::IsClassInitializer() const {
  return IsStatic() && GetName()->Equals("<clinit>");
}

// TODO: get global references for these
Class* Method::java_lang_reflect_Constructor_ = NULL;
Class* Method::java_lang_reflect_Method_ = NULL;

void Method::SetClasses(Class* java_lang_reflect_Constructor, Class* java_lang_reflect_Method) {
  CHECK(java_lang_reflect_Constructor_ == NULL);
  CHECK(java_lang_reflect_Constructor != NULL);
  java_lang_reflect_Constructor_ = java_lang_reflect_Constructor;

  CHECK(java_lang_reflect_Method_ == NULL);
  CHECK(java_lang_reflect_Method != NULL);
  java_lang_reflect_Method_ = java_lang_reflect_Method;
}

void Method::ResetClasses() {
  CHECK(java_lang_reflect_Constructor_ != NULL);
  java_lang_reflect_Constructor_ = NULL;

  CHECK(java_lang_reflect_Method_ != NULL);
  java_lang_reflect_Method_ = NULL;
}

Class* ExtractNextClassFromSignature(ClassLinker* class_linker, const ClassLoader* cl, const char*& p) {
  if (*p == '[') {
    // Something like "[[[Ljava/lang/String;".
    const char* start = p;
    while (*p == '[') {
      ++p;
    }
    if (*p == 'L') {
      while (*p != ';') {
        ++p;
      }
    }
    ++p; // Either the ';' or the primitive type.

    std::string descriptor(start, (p - start));
    return class_linker->FindClass(descriptor, cl);
  } else if (*p == 'L') {
    const char* start = p;
    while (*p != ';') {
      ++p;
    }
    ++p;
    StringPiece descriptor(start, (p - start));
    return class_linker->FindClass(descriptor.ToString(), cl);
  } else {
    return class_linker->FindPrimitiveClass(*p++);
  }
}

void Method::InitJavaFieldsLocked() {
  // Create the array.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  size_t arg_count = GetShorty()->GetLength() - 1;
  Class* array_class = class_linker->FindSystemClass("[Ljava/lang/Class;");
  ObjectArray<Class>* parameters = ObjectArray<Class>::Alloc(array_class, arg_count);
  if (parameters == NULL) {
    return;
  }

  // Parse the signature, filling the array.
  const ClassLoader* cl = GetDeclaringClass()->GetClassLoader();
  std::string signature(GetSignature()->ToModifiedUtf8());
  const char* p = signature.c_str();
  DCHECK_EQ(*p, '(');
  ++p;
  for (size_t i = 0; i < arg_count; ++i) {
    Class* c = ExtractNextClassFromSignature(class_linker, cl, p);
    if (c == NULL) {
      return;
    }
    parameters->Set(i, c);
  }

  DCHECK_EQ(*p, ')');
  ++p;

  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, java_parameter_types_),
                 parameters, false);
  Class* java_return_type = ExtractNextClassFromSignature(class_linker, cl, p);
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, java_return_type_),
                 java_return_type, false);
}

void Method::InitJavaFields() {
  Thread* self = Thread::Current();
  ScopedThreadStateChange tsc(self, Thread::kRunnable);
  MonitorEnter(self);
  if (java_parameter_types_ == NULL || java_return_type_ == NULL) {
    InitJavaFieldsLocked();
  }
  MonitorExit(self);
}

ObjectArray<String>* Method::GetDexCacheStrings() const {
  return GetFieldObject<ObjectArray<String>*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_strings_), false);
}

void Method::SetReturnTypeIdx(uint32_t new_return_type_idx) {
  SetField32(OFFSET_OF_OBJECT_MEMBER(Method, java_return_type_idx_),
             new_return_type_idx, false);
}

uint32_t Method::GetDexMethodIndex() const {
  // TODO: add the method index to Method - which will also mean a number of Method fields can
  // become dex file lookups (which will then mean we may want faster access to the dex file)
  // Find the dex file
  const DexCache* dex_cache = GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
  // Find the class_def in the dex file
  uint32_t class_def_idx;
  bool found_class_def =
      dex_file.FindClassDefIndex(GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8(),
                                 class_def_idx);
  CHECK(found_class_def);
  const DexFile::TypeId& type_id =
      dex_file.GetTypeId(dex_file.GetClassDef(class_def_idx).class_idx_);
  const DexFile::StringId* name_str_id = dex_file.FindStringId(GetName()->ToModifiedUtf8());
  CHECK(name_str_id != NULL);  // Failed to find method's name?
  uint16_t return_type_idx;
  std::vector<uint16_t> param_type_idxs;
  std::string signature = GetSignature()->ToModifiedUtf8();
  bool found_type_list = dex_file.CreateTypeList(&return_type_idx, &param_type_idxs, signature);
  CHECK(found_type_list);   // Failed to parse signature
  const DexFile::ProtoId* sig_proto_id = dex_file.FindProtoId(return_type_idx, param_type_idxs);
  CHECK(sig_proto_id != NULL);  // Failed to find method's prototype
  const DexFile::MethodId* method_id =
      dex_file.FindMethodId(type_id, *name_str_id, *sig_proto_id);
  CHECK(method_id != NULL);  // Failed to find method?
  uint32_t method_idx = dex_file.GetIndexForMethodId(*method_id);
  DCHECK_EQ(PrettyMethod(method_idx, dex_file), PrettyMethod(this));
  return method_idx;
}

const char* Method::GetReturnTypeDescriptor() const {
  Class* declaring_class = GetDeclaringClass();
  DexCache* dex_cache = declaring_class->GetDexCache();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
  const char* descriptor = dex_file.StringByTypeIdx(GetReturnTypeIdx());
  DCHECK(descriptor != NULL);
  return descriptor;
}

Class* Method::GetReturnType() const {
  DCHECK(GetDeclaringClass()->IsResolved() || GetDeclaringClass()->IsErroneous())
      << PrettyMethod(this);
  Class* java_return_type = java_return_type_;
  if (java_return_type != NULL) {
      return java_return_type;
  }
  // Short-cut
  Class* result = GetDexCacheResolvedTypes()->Get(GetReturnTypeIdx());
  if (result == NULL) {
    // Do full linkage and set cache value for next call
    result = Runtime::Current()->GetClassLinker()->ResolveType(GetReturnTypeIdx(), this);
  }
  CHECK(result != NULL) << PrettyMethod(this);
  return result;
}

void Method::SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_strings_),
                 new_dex_cache_strings, false);
}

ObjectArray<Class>* Method::GetDexCacheResolvedTypes() const {
  return GetFieldObject<ObjectArray<Class>*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_types_), false);
}

void Method::SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_classes) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_types_),
                 new_dex_cache_classes, false);
}

ObjectArray<Method>* Method::GetDexCacheResolvedMethods() const {
  return GetFieldObject<ObjectArray<Method>*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_methods_), false);
}

void Method::SetDexCacheResolvedMethods(ObjectArray<Method>* new_dex_cache_methods) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_methods_),
                 new_dex_cache_methods, false);
}

ObjectArray<Field>* Method::GetDexCacheResolvedFields() const {
  return GetFieldObject<ObjectArray<Field>*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_fields_), false);
}

void Method::SetDexCacheResolvedFields(ObjectArray<Field>* new_dex_cache_fields) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_resolved_fields_),
                 new_dex_cache_fields, false);
}

CodeAndDirectMethods* Method::GetDexCacheCodeAndDirectMethods() const {
  return GetFieldPtr<CodeAndDirectMethods*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_code_and_direct_methods_),
      false);
}

void Method::SetDexCacheCodeAndDirectMethods(CodeAndDirectMethods* new_value) {
  SetFieldPtr<CodeAndDirectMethods*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_code_and_direct_methods_),
      new_value, false);
}

ObjectArray<StaticStorageBase>* Method::GetDexCacheInitializedStaticStorage() const {
  return GetFieldObject<ObjectArray<StaticStorageBase>*>(
      OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_initialized_static_storage_),
      false);
}

void Method::SetDexCacheInitializedStaticStorage(ObjectArray<StaticStorageBase>* new_value) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Method, dex_cache_initialized_static_storage_),
      new_value, false);
}

size_t Method::NumArgRegisters(const StringPiece& shorty) {
  CHECK_LE(1, shorty.length());
  uint32_t num_registers = 0;
  for (int i = 1; i < shorty.length(); ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

size_t Method::NumArgs() const {
  // "1 +" because the first in Args is the receiver.
  // "- 1" because we don't count the return type.
  return (IsStatic() ? 0 : 1) + GetShorty()->GetLength() - 1;
}

// The number of reference arguments to this method including implicit this
// pointer
size_t Method::NumReferenceArgs() const {
  const String* shorty = GetShorty();
  size_t result = IsStatic() ? 0 : 1;  // The implicit this pointer.
  for (int i = 1; i < shorty->GetLength(); i++) {
    char ch = shorty->CharAt(i);
    if ((ch == 'L') || (ch == '[')) {
      result++;
    }
  }
  return result;
}

// The number of long or double arguments
size_t Method::NumLongOrDoubleArgs() const {
  const String* shorty = GetShorty();
  size_t result = 0;
  for (int i = 1; i < shorty->GetLength(); i++) {
    char ch = shorty->CharAt(i);
    if ((ch == 'D') || (ch == 'J')) {
      result++;
    }
  }
  return result;
}

// Is the given method parameter a reference?
bool Method::IsParamAReference(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return true;  // this argument
  }
  return GetShorty()->CharAt(param) == 'L';
}

// Is the given method parameter a long or double?
bool Method::IsParamALongOrDouble(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return false;  // this argument
  }
  char ch = GetShorty()->CharAt(param);
  return (ch == 'J' || ch == 'D');
}

static size_t ShortyCharToSize(char x) {
  switch (x) {
    case 'V': return 0;
    case '[': return kPointerSize;
    case 'L': return kPointerSize;
    case 'D': return 8;
    case 'J': return 8;
    default:  return 4;
  }
}

size_t Method::ParamSize(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return kPointerSize;  // this argument
  }
  return ShortyCharToSize(GetShorty()->CharAt(param));
}

size_t Method::ReturnSize() const {
  return ShortyCharToSize(GetShorty()->CharAt(0));
}

Method* Method::FindOverriddenMethod() const {
  if (IsStatic()) {
    return NULL;
  }
  Class* declaring_class = GetDeclaringClass();
  Class* super_class = declaring_class->GetSuperClass();
  uint16_t method_index = GetMethodIndex();
  ObjectArray<Method>* super_class_vtable = super_class->GetVTable();
  Method* result = NULL;
  if (super_class_vtable != NULL && method_index < super_class_vtable->GetLength()) {
    result = super_class_vtable->Get(method_index);
  } else {
    ObjectArray<Class>* interfaces = declaring_class->GetInterfaces();
    String* name = GetName();
    String* signature = GetSignature();
    for (int32_t i = 0; i < interfaces->GetLength() && result == NULL; i++) {
      Class* interface = interfaces->Get(i);
      result = interface->FindInterfaceMethod(name, signature);
    }
  }
  DCHECK(result == NULL || HasSameNameAndSignature(result));
  return result;
}

uint32_t Method::ToDexPC(const uintptr_t pc) const {
  const uint32_t* mapping_table = GetMappingTable();
  if (mapping_table == NULL) {
    DCHECK(IsNative() || IsCalleeSaveMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  size_t mapping_table_length = GetMappingTableLength();
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(GetCode());
  uint32_t best_offset = 0;
  uint32_t best_dex_offset = 0;
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    uint32_t map_offset = mapping_table[i];
    uint32_t map_dex_offset = mapping_table[i + 1];
    if (map_offset == sought_offset) {
      best_offset = map_offset;
      best_dex_offset = map_dex_offset;
      break;
    }
    if (map_offset < sought_offset && map_offset > best_offset) {
      best_offset = map_offset;
      best_dex_offset = map_dex_offset;
    }
  }
  return best_dex_offset;
}

uintptr_t Method::ToNativePC(const uint32_t dex_pc) const {
  const uint32_t* mapping_table = GetMappingTable();
  if (mapping_table == NULL) {
    DCHECK_EQ(dex_pc, 0U);
    return 0;   // Special no mapping/pc == 0 case
  }
  size_t mapping_table_length = GetMappingTableLength();
  for (size_t i = 0; i < mapping_table_length; i += 2) {
    uint32_t map_offset = mapping_table[i];
    uint32_t map_dex_offset = mapping_table[i + 1];
    if (map_dex_offset == dex_pc) {
      return reinterpret_cast<uintptr_t>(GetCode()) + map_offset;
    }
  }
  LOG(FATAL) << "Looking up Dex PC not contained in method";
  return 0;
}

uint32_t Method::FindCatchBlock(Class* exception_type, uint32_t dex_pc) const {
  DexCache* dex_cache = GetDeclaringClass()->GetDexCache();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(GetCodeItemOffset());
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, dex_pc); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return it.GetHandlerAddress();
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = dex_cache->GetResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << dex_file.GetTypeDescriptor(dex_file.GetTypeId(iter_type_idx));
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return it.GetHandlerAddress();
    }
  }
  // Handler not found
  return DexFile::kDexNoIndex;
}

void Method::Invoke(Thread* self, Object* receiver, byte* args, JValue* result) const {
  // Push a transition back into managed code onto the linked list in thread.
  CHECK_EQ(Thread::kRunnable, self->GetState());
  NativeToManagedRecord record;
  self->PushNativeToManagedRecord(&record);

  // Call the invoke stub associated with the method.
  // Pass everything as arguments.
  const Method::InvokeStub* stub = GetInvokeStub();

  bool have_executable_code = (GetCode() != NULL);
#if !defined(__arm__)
  // Currently we can only compile non-native methods for ARM.
  have_executable_code = IsNative();
#endif

  if (have_executable_code && stub != NULL) {
    bool log = false;
    if (log) {
      LOG(INFO) << "invoking " << PrettyMethod(this) << " code=" << (void*) GetCode() << " stub=" << (void*) stub;
    }
    (*stub)(this, receiver, self, args, result);
    if (log) {
      LOG(INFO) << "returned " << PrettyMethod(this) << " code=" << (void*) GetCode() << " stub=" << (void*) stub;
    }
  } else {
    if (Runtime::Current()->IsStarted()) {
      LOG(WARNING) << "Not invoking method with no associated code: " << PrettyMethod(this);
    }
    if (result != NULL) {
      result->j = 0;
    }
  }

  // Pop transition.
  self->PopNativeToManagedRecord(record);
}

bool Method::IsRegistered() const {
  void* native_method = GetFieldPtr<void*>(OFFSET_OF_OBJECT_MEMBER(Method, native_method_), false);
  void* jni_stub = Runtime::Current()->GetJniStubArray()->GetData();
  return native_method != jni_stub;
}

void Method::RegisterNative(const void* native_method) {
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(native_method != NULL) << PrettyMethod(this);
  SetFieldPtr<const void*>(OFFSET_OF_OBJECT_MEMBER(Method, native_method_),
                           native_method, false);
}

void Method::UnregisterNative() {
  CHECK(IsNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(Runtime::Current()->GetJniStubArray()->GetData());
}

void Class::SetStatus(Status new_status) {
  CHECK(new_status > GetStatus() || new_status == kStatusError || !Runtime::Current()->IsStarted())
      << PrettyClass(this) << " " << GetStatus() << " -> " << new_status;
  CHECK(sizeof(Status) == sizeof(uint32_t)) << PrettyClass(this);
  return SetField32(OFFSET_OF_OBJECT_MEMBER(Class, status_), new_status, false);
}

DexCache* Class::GetDexCache() const {
  return GetFieldObject<DexCache*>(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), false);
}

void Class::SetDexCache(DexCache* new_dex_cache) {
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, dex_cache_), new_dex_cache, false);
}

Object* Class::AllocObject() {
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  DCHECK(IsInstantiable()) << PrettyClass(this);
  DCHECK(!Runtime::Current()->IsStarted() || IsInitializing()) << PrettyClass(this);
  DCHECK_GE(this->object_size_, sizeof(Object));
  return Heap::AllocObject(this, this->object_size_);
}

void Class::SetClassSize(size_t new_class_size) {
  DCHECK_GE(new_class_size, GetClassSize()) << " class=" << PrettyTypeOf(this);
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, class_size_), new_class_size, false);
}

void Class::DumpClass(std::ostream& os, int flags) const {
  if ((flags & kDumpClassFullDetail) == 0) {
    os << PrettyClass(this);
    if ((flags & kDumpClassClassLoader) != 0) {
      os << ' ' << GetClassLoader();
    }
    if ((flags & kDumpClassInitialized) != 0) {
      os << ' ' << GetStatus();
    }
    os << "\n";
    return;
  }

  Class* super = GetSuperClass();
  os << "----- " << (IsInterface() ? "interface" : "class") << " "
     << "'" << GetDescriptor()->ToModifiedUtf8() << "' cl=" << GetClassLoader() << " -----\n",
  os << "  objectSize=" << SizeOf() << " "
     << "(" << (super != NULL ? super->SizeOf() : -1) << " from super)\n",
  os << StringPrintf("  access=0x%04x.%04x\n",
      GetAccessFlags() >> 16, GetAccessFlags() & kAccJavaFlagsMask);
  if (super != NULL) {
    os << "  super='" << PrettyClass(super) << "' (cl=" << super->GetClassLoader() << ")\n";
  }
  if (IsArrayClass()) {
    os << "  componentType=" << PrettyClass(GetComponentType()) << "\n";
  }
  if (NumInterfaces() > 0) {
    os << "  interfaces (" << NumInterfaces() << "):\n";
    for (size_t i = 0; i < NumInterfaces(); ++i) {
      Class* interface = GetInterface(i);
      const ClassLoader* cl = interface->GetClassLoader();
      os << StringPrintf("    %2d: %s (cl=%p)\n", i, PrettyClass(interface).c_str(), cl);
    }
  }
  os << "  vtable (" << NumVirtualMethods() << " entries, "
     << (super != NULL ? super->NumVirtualMethods() : 0) << " in super):\n";
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    os << StringPrintf("    %2d: %s\n", i, PrettyMethod(GetVirtualMethodDuringLinking(i)).c_str());
  }
  os << "  direct methods (" << NumDirectMethods() << " entries):\n";
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    os << StringPrintf("    %2d: %s\n", i, PrettyMethod(GetDirectMethod(i)).c_str());
  }
  if (NumStaticFields() > 0) {
    os << "  static fields (" << NumStaticFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumStaticFields(); ++i) {
        os << StringPrintf("    %2d: %s\n", i, PrettyField(GetStaticField(i)).c_str());
      }
    } else {
      os << "    <not yet available>";
    }
  }
  if (NumInstanceFields() > 0) {
    os << "  instance fields (" << NumInstanceFields() << " entries):\n";
    if (IsResolved() || IsErroneous()) {
      for (size_t i = 0; i < NumInstanceFields(); ++i) {
        os << StringPrintf("    %2d: %s\n", i, PrettyField(GetInstanceField(i)).c_str());
      }
    } else {
      os << "    <not yet available>";
    }
  }
}

void Class::SetReferenceInstanceOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    Class* cur = this;
    size_t cnt = 0;
    while (cur) {
      cnt += cur->NumReferenceInstanceFieldsDuringLinking();
      cur = cur->GetSuperClass();
    }
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets), cnt);
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_instance_offsets_),
             new_reference_offsets, false);
}

void Class::SetReferenceStaticOffsets(uint32_t new_reference_offsets) {
  if (new_reference_offsets != CLASS_WALK_SUPER) {
    // Sanity check that the number of bits set in the reference offset bitmap
    // agrees with the number of references
    CHECK_EQ((size_t)__builtin_popcount(new_reference_offsets),
             NumReferenceStaticFieldsDuringLinking());
  }
  SetField32(OFFSET_OF_OBJECT_MEMBER(Class, reference_static_offsets_),
             new_reference_offsets, false);
}

bool Class::Implements(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(klass->IsInterface()) << PrettyClass(this);
  // All interfaces implemented directly and by our superclass, and
  // recursively all super-interfaces of those interfaces, are listed
  // in iftable_, so we can just do a linear scan through that.
  int32_t iftable_count = GetIfTableCount();
  ObjectArray<InterfaceEntry>* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    if (iftable->Get(i)->GetInterface() == klass) {
      return true;
    }
  }
  return false;
}

// Determine whether "this" is assignable from "klazz", where both of these
// are array classes.
//
// Consider an array class, e.g. Y[][], where Y is a subclass of X.
//   Y[][]            = Y[][] --> true (identity)
//   X[][]            = Y[][] --> true (element superclass)
//   Y                = Y[][] --> false
//   Y[]              = Y[][] --> false
//   Object           = Y[][] --> true (everything is an object)
//   Object[]         = Y[][] --> true
//   Object[][]       = Y[][] --> true
//   Object[][][]     = Y[][] --> false (too many []s)
//   Serializable     = Y[][] --> true (all arrays are Serializable)
//   Serializable[]   = Y[][] --> true
//   Serializable[][] = Y[][] --> false (unless Y is Serializable)
//
// Don't forget about primitive types.
//   Object[]         = int[] --> false
//
bool Class::IsArrayAssignableFromArray(const Class* src) const {
  DCHECK(IsArrayClass())  << PrettyClass(this);
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  return GetComponentType()->IsAssignableFrom(src->GetComponentType());
}

bool Class::IsAssignableFromArray(const Class* src) const {
  DCHECK(!IsInterface()) << PrettyClass(this);  // handled first in IsAssignableFrom
  DCHECK(src->IsArrayClass()) << PrettyClass(src);
  if (!IsArrayClass()) {
    // If "this" is not also an array, it must be Object.
    // src's super should be java_lang_Object, since it is an array.
    Class* java_lang_Object = src->GetSuperClass();
    DCHECK(java_lang_Object != NULL) << PrettyClass(src);
    DCHECK(java_lang_Object->GetSuperClass() == NULL) << PrettyClass(src);
    return this == java_lang_Object;
  }
  return IsArrayAssignableFromArray(src);
}

bool Class::IsSubClass(const Class* klass) const {
  DCHECK(!IsInterface()) << PrettyClass(this);
  DCHECK(!IsArrayClass()) << PrettyClass(this);
  const Class* current = this;
  do {
    if (current == klass) {
      return true;
    }
    current = current->GetSuperClass();
  } while (current != NULL);
  return false;
}

bool Class::IsInSamePackage(const String* descriptor_string_1,
                            const String* descriptor_string_2) {
  const std::string descriptor1(descriptor_string_1->ToModifiedUtf8());
  const std::string descriptor2(descriptor_string_2->ToModifiedUtf8());

  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (descriptor1.find('/', i) != StringPiece::npos ||
      descriptor2.find('/', i) != StringPiece::npos) {
    return false;
  } else {
    return true;
  }
}

#if 0
bool Class::IsInSamePackage(const StringPiece& descriptor1,
                            const StringPiece& descriptor2) {
  size_t size = std::min(descriptor1.size(), descriptor2.size());
  std::pair<StringPiece::const_iterator, StringPiece::const_iterator> pos;
  pos = std::mismatch(descriptor1.begin(), descriptor1.begin() + size,
                      descriptor2.begin());
  return !(*(pos.second).rfind('/') != npos && descriptor2.rfind('/') != npos);
}
#endif

bool Class::IsInSamePackage(const Class* that) const {
  const Class* klass1 = this;
  const Class* klass2 = that;
  if (klass1 == klass2) {
    return true;
  }
  // Class loaders must match.
  if (klass1->GetClassLoader() != klass2->GetClassLoader()) {
    return false;
  }
  // Arrays are in the same package when their element classes are.
  while (klass1->IsArrayClass()) {
    klass1 = klass1->GetComponentType();
  }
  while (klass2->IsArrayClass()) {
    klass2 = klass2->GetComponentType();
  }
  // Compare the package part of the descriptor string.
  return IsInSamePackage(klass1->descriptor_, klass2->descriptor_);
}

const ClassLoader* Class::GetClassLoader() const {
  return GetFieldObject<const ClassLoader*>(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), false);
}

void Class::SetClassLoader(const ClassLoader* new_cl) {
  ClassLoader* new_class_loader = const_cast<ClassLoader*>(new_cl);
  SetFieldObject(OFFSET_OF_OBJECT_MEMBER(Class, class_loader_), new_class_loader, false);
}

Method* Class::FindVirtualMethodForInterface(Method* method, bool can_throw) {
  Class* declaring_class = method->GetDeclaringClass();
  DCHECK(declaring_class != NULL) << PrettyClass(this);
  DCHECK(declaring_class->IsInterface()) << PrettyMethod(method);
  // TODO cache to improve lookup speed
  int32_t iftable_count = GetIfTableCount();
  ObjectArray<InterfaceEntry>* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    InterfaceEntry* interface_entry = iftable->Get(i);
    if (interface_entry->GetInterface() == declaring_class) {
      return interface_entry->GetMethodArray()->Get(method->GetMethodIndex());
    }
  }
  if (can_throw) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
        "Class %s does not implement interface %s",
        PrettyDescriptor(GetDescriptor()).c_str(),
        PrettyDescriptor(declaring_class->GetDescriptor()).c_str());
  }
  return NULL;
}

Method* Class::FindInterfaceMethod(const StringPiece& name,  const StringPiece& signature) const {
  // Check the current class before checking the interfaces.
  Method* method = FindVirtualMethod(name, signature);
  if (method != NULL) {
    return method;
  }

  int32_t iftable_count = GetIfTableCount();
  ObjectArray<InterfaceEntry>* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    method = iftable->Get(i)->GetInterface()->FindVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindInterfaceMethod(String* name,  String* signature) const {
  // Check the current class before checking the interfaces.
  Method* method = FindVirtualMethod(name, signature);
  if (method != NULL) {
    return method;
  }
  int32_t iftable_count = GetIfTableCount();
  ObjectArray<InterfaceEntry>* iftable = GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    Class* interface = iftable->Get(i)->GetInterface();
    method = interface->FindVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDeclaredDirectMethod(const StringPiece& name,
                                        const StringPiece& signature) {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    Method* method = GetDirectMethod(i);
    if (method->GetName()->Equals(name) &&
        method->GetSignature()->Equals(signature)) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDirectMethod(const StringPiece& name,
                                const StringPiece& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                         const StringPiece& signature) const {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    Method* method = GetVirtualMethod(i);
    if (method->GetName()->Equals(name) && method->GetSignature()->Equals(signature)) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDeclaredVirtualMethod(String* name, String* signature) const {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    Method* method = GetVirtualMethod(i);
    if (method->GetName() == name && method->GetSignature() == signature) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindVirtualMethod(const StringPiece& name, const StringPiece& signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindVirtualMethod(String* name, String* signature) const {
  for (const Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredVirtualMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    Field* f = GetInstanceField(i);
    if (f->GetName()->Equals(name) &&
        StringPiece(f->GetTypeDescriptor()) == type) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredInstanceField(String* name, String* type) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    Field* f = GetInstanceField(i);
    if (f->GetName() == name && type->Equals(f->GetTypeDescriptor())) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(String* name, String* type) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(name, type);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& type) {
  DCHECK(type != NULL);
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    Field* f = GetStaticField(i);
    if (f->GetName()->Equals(name) && StringPiece(f->GetTypeDescriptor()) == type) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(String* name, String* type) {
  DCHECK(type != NULL);
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    Field* f = GetStaticField(i);
    if (f->GetName() == name && type->Equals(f->GetTypeDescriptor())) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const StringPiece& name, const StringPiece& type) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    // Is the field in this class?
    Field* f = c->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }

    // Is this field in any of this class' interfaces?
    for (int32_t i = 0; i < c->GetIfTableCount(); ++i) {
      InterfaceEntry* interface_entry = c->GetIfTable()->Get(i);
      Class* interface = interface_entry->GetInterface();
      f = interface->FindDeclaredStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Field* Class::FindStaticField(String* name, String* type) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    // Is the field in this class?
    Field* f = c->FindDeclaredStaticField(name, type);
    if (f != NULL) {
      return f;
    }

    // Is this field in any of this class' interfaces?
    for (int32_t i = 0; i < c->GetIfTableCount(); ++i) {
      InterfaceEntry* interface_entry = c->GetIfTable()->Get(i);
      Class* interface = interface_entry->GetInterface();
      f = interface->FindDeclaredStaticField(name, type);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

Array* Array::Alloc(Class* array_class, int32_t component_count, size_t component_size) {
  DCHECK(array_class != NULL);
  DCHECK_GE(component_count, 0);
  DCHECK(array_class->IsArrayClass());

  size_t header_size = sizeof(Array);
  size_t data_size = component_count * component_size;
  size_t size = header_size + data_size;

  // Check for overflow and throw OutOfMemoryError if this was an unreasonable request.
  size_t component_shift = sizeof(size_t) * 8 - 1 - CLZ(component_size);
  if (data_size >> component_shift != size_t(component_count) || size < data_size) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/OutOfMemoryError;",
        "%s of length %zd exceeds the VM limit",
        PrettyDescriptor(array_class->GetDescriptor()).c_str(), component_count);
    return NULL;
  }

  Array* array = down_cast<Array*>(Heap::AllocObject(array_class, size));
  if (array != NULL) {
    DCHECK(array->IsArrayInstance());
    array->SetLength(component_count);
  }
  return array;
}

Array* Array::Alloc(Class* array_class, int32_t component_count) {
  return Alloc(array_class, component_count, array_class->GetComponentSize());
}

bool Array::ThrowArrayIndexOutOfBoundsException(int32_t index) const {
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
      "length=%i; index=%i", length_, index);
  return false;
}

bool Array::ThrowArrayStoreException(Object* object) const {
  Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
      "Can't store an element of type %s into an array of type %s",
      PrettyTypeOf(object).c_str(), PrettyTypeOf(this).c_str());
  return false;
}

template<typename T>
PrimitiveArray<T>* PrimitiveArray<T>::Alloc(size_t length) {
  DCHECK(array_class_ != NULL);
  Array* raw_array = Array::Alloc(array_class_, length, sizeof(T));
  return down_cast<PrimitiveArray<T>*>(raw_array);
}

template <typename T> Class* PrimitiveArray<T>::array_class_ = NULL;

// Explicitly instantiate all the primitive array types.
template class PrimitiveArray<uint8_t>;   // BooleanArray
template class PrimitiveArray<int8_t>;    // ByteArray
template class PrimitiveArray<uint16_t>;  // CharArray
template class PrimitiveArray<double>;    // DoubleArray
template class PrimitiveArray<float>;     // FloatArray
template class PrimitiveArray<int32_t>;   // IntArray
template class PrimitiveArray<int64_t>;   // LongArray
template class PrimitiveArray<int16_t>;   // ShortArray

// Explicitly instantiate Class[][]
template class ObjectArray<ObjectArray<Class> >;

// TODO: get global references for these
Class* String::java_lang_String_ = NULL;

void String::SetClass(Class* java_lang_String) {
  CHECK(java_lang_String_ == NULL);
  CHECK(java_lang_String != NULL);
  java_lang_String_ = java_lang_String;
}

void String::ResetClass() {
  CHECK(java_lang_String_ != NULL);
  java_lang_String_ = NULL;
}

String* String::Intern() {
  return Runtime::Current()->GetInternTable()->InternWeak(this);
}

int32_t String::GetHashCode() {
  int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), false);
  if (result == 0) {
    ComputeHashCode();
  }
  result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, hash_code_), false);
  DCHECK(result != 0 || ComputeUtf16Hash(GetCharArray(), GetOffset(), GetLength()) == 0)
          << ToModifiedUtf8() << " " << result;
  return result;
}

int32_t String::GetLength() const {
  int32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(String, count_), false);
  DCHECK(result >= 0 && result <= GetCharArray()->GetLength());
  return result;
}

uint16_t String::CharAt(int32_t index) const {
  // TODO: do we need this? Equals is the only caller, and could
  // bounds check itself.
  if (index < 0 || index >= count_) {
    Thread* self = Thread::Current();
    self->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
        "length=%i; index=%i", count_, index);
    return 0;
  }
  return GetCharArray()->Get(index + GetOffset());
}

String* String::AllocFromUtf16(int32_t utf16_length,
                               const uint16_t* utf16_data_in,
                               int32_t hash_code) {
  String* string = Alloc(GetJavaLangString(), utf16_length);
  if (string == NULL) {
    return NULL;
  }
  // TODO: use 16-bit wide memset variant
  CharArray* array = const_cast<CharArray*>(string->GetCharArray());
  if (array == NULL) {
    return NULL;
  }
  for (int i = 0; i < utf16_length; i++) {
    array->Set(i, utf16_data_in[i]);
  }
  if (hash_code != 0) {
    string->SetHashCode(hash_code);
  } else {
    string->ComputeHashCode();
  }
  return string;
}

String* String::AllocFromModifiedUtf8(const char* utf) {
  size_t char_count = CountModifiedUtf8Chars(utf);
  return AllocFromModifiedUtf8(char_count, utf);
}

String* String::AllocFromModifiedUtf8(int32_t utf16_length,
                                      const char* utf8_data_in) {
  String* string = Alloc(GetJavaLangString(), utf16_length);
  if (string == NULL) {
    return NULL;
  }
  uint16_t* utf16_data_out =
      const_cast<uint16_t*>(string->GetCharArray()->GetData());
  ConvertModifiedUtf8ToUtf16(utf16_data_out, utf8_data_in);
  string->ComputeHashCode();
  return string;
}

String* String::Alloc(Class* java_lang_String, int32_t utf16_length) {
  SirtRef<CharArray> array(CharArray::Alloc(utf16_length));
  if (array.get() == NULL) {
    return NULL;
  }
  return Alloc(java_lang_String, array.get());
}

String* String::Alloc(Class* java_lang_String, CharArray* array) {
  SirtRef<CharArray> array_ref(array);  // hold reference in case AllocObject causes GC
  String* string = down_cast<String*>(java_lang_String->AllocObject());
  if (string == NULL) {
    return NULL;
  }
  string->SetArray(array);
  string->SetCount(array->GetLength());
  return string;
}

bool String::Equals(const String* that) const {
  if (this == that) {
    // Quick reference equality test
    return true;
  } else if (that == NULL) {
    // Null isn't an instanceof anything
    return false;
  } else if (this->GetLength() != that->GetLength()) {
    // Quick length inequality test
    return false;
  } else {
    // Note: don't short circuit on hash code as we're presumably here as the
    // hash code was already equal
    for (int32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const uint16_t* that_chars, int32_t that_offset,
                    int32_t that_length) const {
  if (this->GetLength() != that_length) {
    return false;
  } else {
    for (int32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const char* modified_utf8) const {
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&modified_utf8);
    if (ch == '\0' || ch != CharAt(i)) {
      return false;
    }
  }
  return *modified_utf8 == '\0';
}

bool String::Equals(const StringPiece& modified_utf8) const {
  if (modified_utf8.size() != GetLength()) {
    return false;
  }
  const char* p = modified_utf8.data();
  for (int32_t i = 0; i < GetLength(); ++i) {
    uint16_t ch = GetUtf16FromUtf8(&p);
    if (ch != CharAt(i)) {
      return false;
    }
  }
  return true;
}

// Create a modified UTF-8 encoded std::string from a java/lang/String object.
std::string String::ToModifiedUtf8() const {
  const uint16_t* chars = GetCharArray()->GetData() + GetOffset();
  size_t byte_count(CountUtf8Bytes(chars, GetLength()));
  std::string result(byte_count, char(0));
  ConvertUtf16ToModifiedUtf8(&result[0], chars, GetLength());
  return result;
}

bool Throwable::IsCheckedException() const {
  Class* error = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/Error;");
  if (InstanceOf(error)) {
    return false;
  }
  Class* jlre = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/RuntimeException;");
  return !InstanceOf(jlre);
}

std::string Throwable::Dump() const {
  Object* stack_state = GetStackState();
  if (stack_state == NULL || !stack_state->IsObjectArray()) {
    // missing or corrupt stack state
    return "";
  }
  // Decode the internal stack trace into the depth and method trace
  ObjectArray<Object>* method_trace = down_cast<ObjectArray<Object>*>(stack_state);
  int32_t depth = method_trace->GetLength() - 1;
  std::string result;
  for (int32_t i = 0; i < depth; ++i) {
    Method* method = down_cast<Method*>(method_trace->Get(i));
    result += "  at ";
    result += PrettyMethod(method, true);
    result += "\n";
  }
  return result;
}

Class* StackTraceElement::java_lang_StackTraceElement_ = NULL;

void StackTraceElement::SetClass(Class* java_lang_StackTraceElement) {
  CHECK(java_lang_StackTraceElement_ == NULL);
  CHECK(java_lang_StackTraceElement != NULL);
  java_lang_StackTraceElement_ = java_lang_StackTraceElement;
}

void StackTraceElement::ResetClass() {
  CHECK(java_lang_StackTraceElement_ != NULL);
  java_lang_StackTraceElement_ = NULL;
}

StackTraceElement* StackTraceElement::Alloc(const String* declaring_class,
                                            const String* method_name,
                                            const String* file_name,
                                            int32_t line_number) {
  StackTraceElement* trace =
      down_cast<StackTraceElement*>(GetStackTraceElement()->AllocObject());
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, declaring_class_),
                        const_cast<String*>(declaring_class), false);
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, method_name_),
                        const_cast<String*>(method_name), false);
  trace->SetFieldObject(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, file_name_),
                        const_cast<String*>(file_name), false);
  trace->SetField32(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, line_number_),
                    line_number, false);
  return trace;
}

static const char* kClassStatusNames[] = {
  "Error",
  "NotReady",
  "Idx",
  "Loaded",
  "Resolved",
  "Verifying",
  "Verified",
  "Initializing",
  "Initialized"
};
std::ostream& operator<<(std::ostream& os, const Class::Status& rhs) {
  if (rhs >= Class::kStatusError && rhs <= Class::kStatusInitialized) {
    os << kClassStatusNames[rhs + 1];
  } else {
    os << "Class::Status[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

}  // namespace art
