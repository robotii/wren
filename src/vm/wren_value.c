#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "wren.h"
#include "wren_value.h"
#include "wren_vm.h"

#if WREN_DEBUG_TRACE_MEMORY
  #include "wren_debug.h"
#endif

// TODO: Tune these.
// The initial (and minimum) capacity of a non-empty list or map object.
#define MIN_CAPACITY 16

// The rate at which a collection's capacity grows when the size exceeds the
// current capacity. The new capacity will be determined by *multiplying* the
// old capacity by this. Growing geometrically is necessary to ensure that
// adding to a collection has O(1) amortized complexity.
#define GROW_FACTOR 2

// The maximum percentage of map entries that can be filled before the map is
// grown. A lower load takes more memory but reduces collisions which makes
// lookup faster.
#define MAP_LOAD_PERCENT 75

DEFINE_BUFFER(Value, Value);
DEFINE_BUFFER(Method, Method);

static void initObj(WrenVM* vm, Obj* obj, ObjType type, ObjClass* classObj)
{
  obj->type = type;
  obj->marked = false;
  obj->classObj = classObj;
  obj->next = vm->first;
  vm->first = obj;
}

ObjClass* wrenNewSingleClass(WrenVM* vm, int numFields, ObjString* name)
{
  ObjClass* classObj = ALLOCATE(vm, ObjClass);
  initObj(vm, &classObj->obj, OBJ_CLASS, NULL);
  classObj->superclass = NULL;
  classObj->numFields = numFields;
  classObj->name = name;

  wrenPushRoot(vm, (Obj*)classObj);
  wrenMethodBufferInit(&classObj->methods);
  wrenPopRoot(vm);

  return classObj;
}

void wrenBindSuperclass(WrenVM* vm, ObjClass* subclass, ObjClass* superclass)
{
  ASSERT(superclass != NULL, "Must have superclass.");

  subclass->superclass = superclass;

  // Include the superclass in the total number of fields.
  subclass->numFields += superclass->numFields;

  // Inherit methods from its superclass.
  for (int i = 0; i < superclass->methods.count; i++)
  {
    wrenBindMethod(vm, subclass, i, superclass->methods.data[i]);
  }
}

ObjClass* wrenNewClass(WrenVM* vm, ObjClass* superclass, int numFields,
                       ObjString* name)
{
  // Create the metaclass.
  Value metaclassName = wrenStringFormat(vm, "@ metaclass", OBJ_VAL(name));
  wrenPushRoot(vm, AS_OBJ(metaclassName));

  ObjClass* metaclass = wrenNewSingleClass(vm, 0, AS_STRING(metaclassName));
  metaclass->obj.classObj = vm->classClass;

  wrenPopRoot(vm);

  // Make sure the metaclass isn't collected when we allocate the class.
  wrenPushRoot(vm, (Obj*)metaclass);

  // Metaclasses always inherit Class and do not parallel the non-metaclass
  // hierarchy.
  wrenBindSuperclass(vm, metaclass, vm->classClass);

  ObjClass* classObj = wrenNewSingleClass(vm, numFields, name);

  // Make sure the class isn't collected while the inherited methods are being
  // bound.
  wrenPushRoot(vm, (Obj*)classObj);

  classObj->obj.classObj = metaclass;
  wrenBindSuperclass(vm, classObj, superclass);

  wrenPopRoot(vm);
  wrenPopRoot(vm);

  return classObj;
}

void wrenBindMethod(WrenVM* vm, ObjClass* classObj, int symbol, Method method)
{
  // Make sure the buffer is big enough to contain the symbol's index.
  if (symbol >= classObj->methods.count)
  {
    Method noMethod;
    noMethod.type = METHOD_NONE;
    wrenMethodBufferFill(vm, &classObj->methods, noMethod,
                         symbol - classObj->methods.count + 1);
  }

  classObj->methods.data[symbol] = method;
}

ObjClosure* wrenNewClosure(WrenVM* vm, ObjFn* fn)
{
  ObjClosure* closure = ALLOCATE_FLEX(vm, ObjClosure,
                                      ObjUpvalue*, fn->numUpvalues);
  initObj(vm, &closure->obj, OBJ_CLOSURE, vm->fnClass);

  closure->fn = fn;

  // Clear the upvalue array. We need to do this in case a GC is triggered
  // after the closure is created but before the upvalue array is populated.
  for (int i = 0; i < fn->numUpvalues; i++) closure->upvalues[i] = NULL;

  return closure;
}

ObjFiber* wrenNewFiber(WrenVM* vm, Obj* fn)
{
  ObjFiber* fiber = ALLOCATE(vm, ObjFiber);
  initObj(vm, &fiber->obj, OBJ_FIBER, vm->fiberClass);
  fiber->id = vm->nextFiberId++;

  wrenResetFiber(fiber, fn);

  return fiber;
}

void wrenResetFiber(ObjFiber* fiber, Obj* fn)
{
  // Push the stack frame for the function.
  fiber->stackTop = fiber->stack;
  fiber->numFrames = 1;
  fiber->openUpvalues = NULL;
  fiber->caller = NULL;
  fiber->error = NULL;
  fiber->callerIsTrying = false;

  CallFrame* frame = &fiber->frames[0];
  frame->fn = fn;
  frame->stackStart = fiber->stack;
  if (fn->type == OBJ_FN)
  {
    frame->ip = ((ObjFn*)fn)->bytecode;
  }
  else
  {
    frame->ip = ((ObjClosure*)fn)->fn->bytecode;
  }
}

ObjFn* wrenNewFunction(WrenVM* vm, ObjModule* module,
                       Value* constants, int numConstants,
                       int numUpvalues, int arity,
                       uint8_t* bytecode, int bytecodeLength,
                       ObjString* debugSourcePath,
                       const char* debugName, int debugNameLength,
                       int* sourceLines)
{
  // Allocate these before the function in case they trigger a GC which would
  // free the function.
  Value* copiedConstants = NULL;
  if (numConstants > 0)
  {
    copiedConstants = ALLOCATE_ARRAY(vm, Value, numConstants);
    for (int i = 0; i < numConstants; i++)
    {
      copiedConstants[i] = constants[i];
    }
  }

  FnDebug* debug = ALLOCATE(vm, FnDebug);

  debug->sourcePath = debugSourcePath;

  // Copy the function's name.
  debug->name = ALLOCATE_ARRAY(vm, char, debugNameLength + 1);
  memcpy(debug->name, debugName, debugNameLength);
  debug->name[debugNameLength] = '\0';

  debug->sourceLines = sourceLines;

  ObjFn* fn = ALLOCATE(vm, ObjFn);
  initObj(vm, &fn->obj, OBJ_FN, vm->fnClass);

  // TODO: Should eventually copy this instead of taking ownership. When the
  // compiler grows this, its capacity will often exceed the actual used size.
  // Copying to an exact-sized buffer will save a bit of memory. I tried doing
  // this, but it made the "for" benchmark ~15% slower for some unknown reason.
  fn->bytecode = bytecode;
  fn->constants = copiedConstants;
  fn->module = module;
  fn->numUpvalues = numUpvalues;
  fn->numConstants = numConstants;
  fn->arity = arity;
  fn->bytecodeLength = bytecodeLength;
  fn->debug = debug;

  return fn;
}

Value wrenNewInstance(WrenVM* vm, ObjClass* classObj)
{
  ObjInstance* instance = ALLOCATE_FLEX(vm, ObjInstance,
                                        Value, classObj->numFields);
  initObj(vm, &instance->obj, OBJ_INSTANCE, classObj);

  // Initialize fields to null.
  for (int i = 0; i < classObj->numFields; i++)
  {
    instance->fields[i] = NULL_VAL;
  }

  return OBJ_VAL(instance);
}

ObjList* wrenNewList(WrenVM* vm, uint32_t numElements)
{
  // Allocate this before the list object in case it triggers a GC which would
  // free the list.
  Value* elements = NULL;
  if (numElements > 0)
  {
    elements = ALLOCATE_ARRAY(vm, Value, numElements);
  }

  ObjList* list = ALLOCATE(vm, ObjList);
  initObj(vm, &list->obj, OBJ_LIST, vm->listClass);
  list->elements.capacity = numElements;
  list->elements.count = numElements;
  list->elements.data = elements;
  return list;
}

void wrenListInsert(WrenVM* vm, ObjList* list, Value value, uint32_t index)
{
  if (IS_OBJ(value)) wrenPushRoot(vm, AS_OBJ(value));

  // Add a slot at the end of the list.
  wrenValueBufferWrite(vm, &list->elements, NULL_VAL);

  if (IS_OBJ(value)) wrenPopRoot(vm);

  // Shift the existing elements down.
  for (uint32_t i = list->elements.count - 1; i > index; i--)
  {
    list->elements.data[i] = list->elements.data[i - 1];
  }

  // Store the new element.
  list->elements.data[index] = value;
}

Value wrenListRemoveAt(WrenVM* vm, ObjList* list, uint32_t index)
{
  Value removed = list->elements.data[index];

  if (IS_OBJ(removed)) wrenPushRoot(vm, AS_OBJ(removed));

  // Shift items up.
  for (int i = index; i < list->elements.count - 1; i++)
  {
    list->elements.data[i] = list->elements.data[i + 1];
  }

  // If we have too much excess capacity, shrink it.
  if (list->elements.capacity / GROW_FACTOR >= list->elements.count)
  {
    list->elements.data = (Value*)wrenReallocate(vm, list->elements.data,
        sizeof(Value) * list->elements.capacity,
        sizeof(Value) * (list->elements.capacity / GROW_FACTOR));
    list->elements.capacity /= GROW_FACTOR;
  }

  if (IS_OBJ(removed)) wrenPopRoot(vm);

  list->elements.count--;
  return removed;
}

ObjMap* wrenNewMap(WrenVM* vm)
{
  ObjMap* map = ALLOCATE(vm, ObjMap);
  initObj(vm, &map->obj, OBJ_MAP, vm->mapClass);
  map->capacity = 0;
  map->count = 0;
  map->entries = NULL;
  return map;
}

// Generates a hash code for [num].
static uint32_t hashNumber(double num)
{
  // Hash the raw bits of the value.
  DoubleBits data;
  data.num = num;
  return data.bits32[0] ^ data.bits32[1];
}

// Generates a hash code for [object].
static uint32_t hashObject(Obj* object)
{
  switch (object->type)
  {
    case OBJ_CLASS:
      // Classes just use their name.
      return hashObject((Obj*)((ObjClass*)object)->name);

    case OBJ_FIBER:
      return ((ObjFiber*)object)->id;

    case OBJ_RANGE:
    {
      ObjRange* range = (ObjRange*)object;
      return hashNumber(range->from) ^ hashNumber(range->to);
    }

    case OBJ_STRING:
      return ((ObjString*)object)->hash;

    default:
      ASSERT(false, "Only immutable objects can be hashed.");
      return 0;
  }
}

// Generates a hash code for [value], which must be one of the built-in
// immutable types: null, bool, class, num, range, or string.
static uint32_t hashValue(Value value)
{
  // TODO: We'll probably want to randomize this at some point.

#if WREN_NAN_TAGGING
  if (IS_OBJ(value)) return hashObject(AS_OBJ(value));

  // Hash the raw bits of the unboxed value.
  DoubleBits bits;

  bits.bits64 = value;
  return bits.bits32[0] ^ bits.bits32[1];
#else
  switch (value.type)
  {
    case VAL_FALSE: return 0;
    case VAL_NULL: return 1;
    case VAL_NUM: return hashNumber(AS_NUM(value));
    case VAL_TRUE: return 2;
    case VAL_OBJ: return hashObject(AS_OBJ(value));
    default:
      UNREACHABLE();
      return 0;
  }
#endif
}

// Inserts [key] and [value] in the array of [entries] with the given
// [capacity].
//
// Returns `true` if this is the first time [key] was added to the map.
static bool addEntry(MapEntry* entries, uint32_t capacity,
                     Value key, Value value)
{
  // Figure out where to insert it in the table. Use open addressing and
  // basic linear probing.
  uint32_t index = hashValue(key) % capacity;

  // We don't worry about an infinite loop here because resizeMap() ensures
  // there are open slots in the array.
  while (true)
  {
    MapEntry* entry = &entries[index];

    // If we found an open slot, the key is not in the table.
    if (IS_UNDEFINED(entry->key))
    {
      // Don't stop at a tombstone, though, because the key may be found after
      // it.
      if (IS_FALSE(entry->value))
      {
        entry->key = key;
        entry->value = value;
        return true;
      }
    }
    else if (wrenValuesEqual(entry->key, key))
    {
      // If the key already exists, just replace the value.
      entry->value = value;
      return false;
    }

    // Try the next slot.
    index = (index + 1) % capacity;
  }
}

// Updates [map]'s entry array to [capacity].
static void resizeMap(WrenVM* vm, ObjMap* map, uint32_t capacity)
{
  // Create the new empty hash table.
  MapEntry* entries = ALLOCATE_ARRAY(vm, MapEntry, capacity);
  for (uint32_t i = 0; i < capacity; i++)
  {
    entries[i].key = UNDEFINED_VAL;
    entries[i].value = FALSE_VAL;
  }

  // Re-add the existing entries.
  if (map->capacity > 0)
  {
    for (uint32_t i = 0; i < map->capacity; i++)
    {
      MapEntry* entry = &map->entries[i];
      if (IS_UNDEFINED(entry->key)) continue;

      addEntry(entries, capacity, entry->key, entry->value);
    }
  }

  // Replace the array.
  DEALLOCATE(vm, map->entries);
  map->entries = entries;
  map->capacity = capacity;
}

static MapEntry *findEntry(ObjMap* map, Value key)
{
  // If there is no entry array (an empty map), we definitely won't find it.
  if (map->capacity == 0) return NULL;

  // Figure out where to insert it in the table. Use open addressing and
  // basic linear probing.
  uint32_t index = hashValue(key) % map->capacity;

  // We don't worry about an infinite loop here because ensureMapCapacity()
  // ensures there are empty (i.e. UNDEFINED) spaces in the table.
  while (true)
  {
    MapEntry* entry = &map->entries[index];

    if (IS_UNDEFINED(entry->key))
    {
      // If we found an empty slot, the key is not in the table. If we found a
      // slot that contains a deleted key, we have to keep looking.
      if (IS_FALSE(entry->value)) return NULL;
    }
    else if (wrenValuesEqual(entry->key, key))
    {
      // If the key matches, we found it.
      return entry;
    }

    // Try the next slot.
    index = (index + 1) % map->capacity;
  }
}

Value wrenMapGet(ObjMap* map, Value key)
{
  MapEntry *entry = findEntry(map, key);
  if (entry != NULL) return entry->value;
  
  return UNDEFINED_VAL;
}

void wrenMapSet(WrenVM* vm, ObjMap* map, Value key, Value value)
{
  // If the map is getting too full, make room first.
  if (map->count + 1 > map->capacity * MAP_LOAD_PERCENT / 100)
  {
    // Figure out the new hash table size.
    uint32_t capacity = map->capacity * GROW_FACTOR;
    if (capacity < MIN_CAPACITY) capacity = MIN_CAPACITY;

    resizeMap(vm, map, capacity);
  }

  if (addEntry(map->entries, map->capacity, key, value))
  {
    // A new key was added.
    map->count++;
  }
}

void wrenMapClear(WrenVM* vm, ObjMap* map)
{
  DEALLOCATE(vm, map->entries);
  map->entries = NULL;
  map->capacity = 0;
  map->count = 0;
}

Value wrenMapRemoveKey(WrenVM* vm, ObjMap* map, Value key)
{
  MapEntry *entry = findEntry(map, key);
  if (entry == NULL) return NULL_VAL;

  // Remove the entry from the map. Set this value to true, which marks it as a
  // deleted slot. When searching for a key, we will stop on empty slots, but
  // continue past deleted slots.
  Value value = entry->value;
  entry->key = UNDEFINED_VAL;
  entry->value = TRUE_VAL;

  if (IS_OBJ(value)) wrenPushRoot(vm, AS_OBJ(value));

  map->count--;

  if (map->count == 0)
  {
    // Removed the last item, so free the array.
    wrenMapClear(vm, map);
  }
  else if (map->capacity > MIN_CAPACITY &&
           map->count < map->capacity / GROW_FACTOR * MAP_LOAD_PERCENT / 100)
  {
    uint32_t capacity = map->capacity / GROW_FACTOR;
    if (capacity < MIN_CAPACITY) capacity = MIN_CAPACITY;

    // The map is getting empty, so shrink the entry array back down.
    // TODO: Should we do this less aggressively than we grow?
    resizeMap(vm, map, capacity);
  }

  if (IS_OBJ(value)) wrenPopRoot(vm);
  return value;
}

ObjModule* wrenNewModule(WrenVM* vm, ObjString* name)
{
  ObjModule* module = ALLOCATE(vm, ObjModule);

  // Modules are never used as first-class objects, so don't need a class.
  initObj(vm, (Obj*)module, OBJ_MODULE, NULL);

  wrenPushRoot(vm, (Obj*)module);

  wrenSymbolTableInit(&module->variableNames);
  wrenValueBufferInit(&module->variables);

  module->name = name;

  wrenPopRoot(vm);
  return module;
}

Value wrenNewRange(WrenVM* vm, double from, double to, bool isInclusive)
{
  ObjRange* range = ALLOCATE(vm, ObjRange);
  initObj(vm, &range->obj, OBJ_RANGE, vm->rangeClass);
  range->from = from;
  range->to = to;
  range->isInclusive = isInclusive;

  return OBJ_VAL(range);
}

// Creates a new string object with a null-terminated buffer large enough to
// hold a string of [length] but does not fill in the bytes.
//
// The caller is expected to fill in the buffer and then calculate the string's
// hash.
static ObjString* allocateString(WrenVM* vm, size_t length)
{
  ObjString* string = ALLOCATE_FLEX(vm, ObjString, char, length + 1);
  initObj(vm, &string->obj, OBJ_STRING, vm->stringClass);
  string->length = (int)length;
  string->value[length] = '\0';

  return string;
}

// Calculates and stores the hash code for [string].
static void hashString(ObjString* string)
{
  // FNV-1a hash. See: http://www.isthe.com/chongo/tech/comp/fnv/
  uint32_t hash = 2166136261u;

  // This is O(n) on the length of the string, but we only call this when a new
  // string is created. Since the creation is also O(n) (to copy/initialize all
  // the bytes), we allow this here.
  for (uint32_t i = 0; i < string->length; i++)
  {
    hash ^= string->value[i];
    hash *= 16777619;
  }

  string->hash = hash;
}

Value wrenNewString(WrenVM* vm, const char* text, size_t length)
{
  // Allow NULL if the string is empty since byte buffers don't allocate any
  // characters for a zero-length string.
  ASSERT(length == 0 || text != NULL, "Unexpected NULL string.");

  ObjString* string = allocateString(vm, length);

  // Copy the string (if given one).
  if (length > 0) memcpy(string->value, text, length);

  hashString(string);

  return OBJ_VAL(string);
}

Value wrenNumToString(WrenVM* vm, double value)
{
  // Corner case: If the value is NaN, different versions of libc produce
  // different outputs (some will format it signed and some won't). To get
  // reliable output, handle that ourselves.
  if (value != value) return CONST_STRING(vm, "nan");
  if (value == INFINITY) return CONST_STRING(vm, "infinity");
  if (value == -INFINITY) return CONST_STRING(vm, "-infinity");

  // This is large enough to hold any double converted to a string using
  // "%.14g". Example:
  //
  //     -1.12345678901234e-1022
  //
  // So we have:
  //
  // + 1 char for sign
  // + 1 char for digit
  // + 1 char for "."
  // + 14 chars for decimal digits
  // + 1 char for "e"
  // + 1 char for "-" or "+"
  // + 4 chars for exponent
  // + 1 char for "\0"
  // = 24
  char buffer[24];
  int length = sprintf(buffer, "%.14g", value);
  return wrenNewString(vm, buffer, length);
}

Value wrenStringFromCodePoint(WrenVM* vm, int value)
{
  int length = wrenUtf8NumBytes(value);
  ASSERT(length != 0, "Value out of range.");

  ObjString* string = allocateString(vm, length);

  wrenUtf8Encode(value, (uint8_t*)string->value);
  hashString(string);

  return OBJ_VAL(string);
}

Value wrenStringFormat(WrenVM* vm, const char* format, ...)
{
  va_list argList;

  // Calculate the length of the result string. Do this up front so we can
  // create the final string with a single allocation.
  va_start(argList, format);
  size_t totalLength = 0;
  for (const char* c = format; *c != '\0'; c++)
  {
    switch (*c)
    {
      case '$':
        totalLength += strlen(va_arg(argList, const char*));
        break;

      case '@':
        totalLength += AS_STRING(va_arg(argList, Value))->length;
        break;

      default:
        // Any other character is interpreted literally.
        totalLength++;
    }
  }
  va_end(argList);

  // Concatenate the string.
  ObjString* result = allocateString(vm, totalLength);

  va_start(argList, format);
  char* start = result->value;
  for (const char* c = format; *c != '\0'; c++)
  {
    switch (*c)
    {
      case '$':
      {
        const char* string = va_arg(argList, const char*);
        size_t length = strlen(string);
        memcpy(start, string, length);
        start += length;
        break;
      }

      case '@':
      {
        ObjString* string = AS_STRING(va_arg(argList, Value));
        memcpy(start, string->value, string->length);
        start += string->length;
        break;
      }

      default:
        // Any other character is interpreted literally.
        *start++ = *c;
    }
  }
  va_end(argList);

  hashString(result);

  return OBJ_VAL(result);
}

Value wrenStringCodePointAt(WrenVM* vm, ObjString* string, uint32_t index)
{
  ASSERT(index < string->length, "Index out of bounds.");

  char first = string->value[index];

  // The first byte's high bits tell us how many bytes are in the UTF-8
  // sequence. If the byte starts with 10xxxxx, it's the middle of a UTF-8
  // sequence, so return an empty string.
  int numBytes;
  if      ((first & 0xc0) == 0x80) numBytes = 0;
  else if ((first & 0xf8) == 0xf0) numBytes = 4;
  else if ((first & 0xf0) == 0xe0) numBytes = 3;
  else if ((first & 0xe0) == 0xc0) numBytes = 2;
  else numBytes = 1;

  return wrenNewString(vm, string->value + index, numBytes);
}

// Uses the Boyer-Moore-Horspool string matching algorithm.
uint32_t wrenStringFind(ObjString* haystack, ObjString* needle)
{
  // Corner case, an empty needle is always found.
  if (needle->length == 0) return 0;

  // If the needle is longer than the haystack it won't be found.
  if (needle->length > haystack->length) return UINT32_MAX;

  // Pre-calculate the shift table. For each character (8-bit value), we
  // determine how far the search window can be advanced if that character is
  // the last character in the haystack where we are searching for the needle
  // and the needle doesn't match there.
  uint32_t shift[UINT8_MAX];
  uint32_t needleEnd = needle->length - 1;

  // By default, we assume the character is not the needle at all. In that case
  // case, if a match fails on that character, we can advance one whole needle
  // width since.
  for (uint32_t index = 0; index < UINT8_MAX; index++)
  {
    shift[index] = needle->length;
  }

  // Then, for every character in the needle, determine how far it is from the
  // end. If a match fails on that character, we can advance the window such
  // that it the last character in it lines up with the last place we could
  // find it in the needle.
  for (uint32_t index = 0; index < needleEnd; index++)
  {
    char c = needle->value[index];
    shift[(uint8_t)c] = needleEnd - index;
  }

  // Slide the needle across the haystack, looking for the first match or
  // stopping if the needle goes off the end.
  char lastChar = needle->value[needleEnd];
  uint32_t range = haystack->length - needle->length;

  for (uint32_t index = 0; index <= range; )
  {
    // Compare the last character in the haystack's window to the last character
    // in the needle. If it matches, see if the whole needle matches.
    char c = haystack->value[index + needleEnd];
    if (lastChar == c &&
        memcmp(haystack->value + index, needle->value, needleEnd) == 0)
    {
      // Found a match.
      return index;
    }

    // Otherwise, slide the needle forward.
    index += shift[(uint8_t)c];
  }

  // Not found.
  return UINT32_MAX;
}

ObjUpvalue* wrenNewUpvalue(WrenVM* vm, Value* value)
{
  ObjUpvalue* upvalue = ALLOCATE(vm, ObjUpvalue);

  // Upvalues are never used as first-class objects, so don't need a class.
  initObj(vm, &upvalue->obj, OBJ_UPVALUE, NULL);

  upvalue->value = value;
  upvalue->closed = NULL_VAL;
  upvalue->next = NULL;
  return upvalue;
}

static void markClass(WrenVM* vm, ObjClass* classObj)
{
  // The metaclass.
  wrenMarkObj(vm, (Obj*)classObj->obj.classObj);

  // The superclass.
  wrenMarkObj(vm, (Obj*)classObj->superclass);

  // Method function objects.
  for (int i = 0; i < classObj->methods.count; i++)
  {
    if (classObj->methods.data[i].type == METHOD_BLOCK)
    {
      wrenMarkObj(vm, classObj->methods.data[i].fn.obj);
    }
  }

  wrenMarkObj(vm, (Obj*)classObj->name);

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjClass);
  vm->bytesAllocated += classObj->methods.capacity * sizeof(Method);
}

static void markClosure(WrenVM* vm, ObjClosure* closure)
{
  // Mark the function.
  wrenMarkObj(vm, (Obj*)closure->fn);

  // Mark the upvalues.
  for (int i = 0; i < closure->fn->numUpvalues; i++)
  {
    wrenMarkObj(vm, (Obj*)closure->upvalues[i]);
  }

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjClosure);
  vm->bytesAllocated += sizeof(ObjUpvalue*) * closure->fn->numUpvalues;
}

static void markFiber(WrenVM* vm, ObjFiber* fiber)
{
  // Stack functions.
  for (int i = 0; i < fiber->numFrames; i++)
  {
    wrenMarkObj(vm, fiber->frames[i].fn);
  }

  // Stack variables.
  for (Value* slot = fiber->stack; slot < fiber->stackTop; slot++)
  {
    wrenMarkValue(vm, *slot);
  }

  // Open upvalues.
  ObjUpvalue* upvalue = fiber->openUpvalues;
  while (upvalue != NULL)
  {
    wrenMarkObj(vm, (Obj*)upvalue);
    upvalue = upvalue->next;
  }

  // The caller.
  wrenMarkObj(vm, (Obj*)fiber->caller);
  wrenMarkObj(vm, (Obj*)fiber->error);

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjFiber);
}

static void markFn(WrenVM* vm, ObjFn* fn)
{
  // Mark the constants.
  for (int i = 0; i < fn->numConstants; i++)
  {
    wrenMarkValue(vm, fn->constants[i]);
  }

  if (fn->debug->sourcePath != NULL)
  {
    wrenMarkObj(vm, (Obj*)fn->debug->sourcePath);
  }

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjFn);
  vm->bytesAllocated += sizeof(uint8_t) * fn->bytecodeLength;
  vm->bytesAllocated += sizeof(Value) * fn->numConstants;

  // The debug line number buffer.
  vm->bytesAllocated += sizeof(int) * fn->bytecodeLength;
  // TODO: What about the function name?
}

static void markInstance(WrenVM* vm, ObjInstance* instance)
{
  wrenMarkObj(vm, (Obj*)instance->obj.classObj);

  // Mark the fields.
  for (int i = 0; i < instance->obj.classObj->numFields; i++)
  {
    wrenMarkValue(vm, instance->fields[i]);
  }

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjInstance);
  vm->bytesAllocated += sizeof(Value) * instance->obj.classObj->numFields;
}

static void markList(WrenVM* vm, ObjList* list)
{
  // Mark the elements.
  wrenMarkBuffer(vm, &list->elements);

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjList);
  vm->bytesAllocated += sizeof(Value) * list->elements.capacity;
}

static void markMap(WrenVM* vm, ObjMap* map)
{
  // Mark the entries.
  for (uint32_t i = 0; i < map->capacity; i++)
  {
    MapEntry* entry = &map->entries[i];
    if (IS_UNDEFINED(entry->key)) continue;

    wrenMarkValue(vm, entry->key);
    wrenMarkValue(vm, entry->value);
  }

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjMap);
  vm->bytesAllocated += sizeof(MapEntry) * map->capacity;
}

static void markModule(WrenVM* vm, ObjModule* module)
{
  // Top-level variables.
  for (int i = 0; i < module->variables.count; i++)
  {
    wrenMarkValue(vm, module->variables.data[i]);
  }

  wrenMarkObj(vm, (Obj*)module->name);

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjModule);
  // TODO: Track memory for symbol table and buffer.
}

static void markRange(WrenVM* vm, ObjRange* range)
{
  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjRange);
}

static void markString(WrenVM* vm, ObjString* string)
{
  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjString) + string->length + 1;
}

static void markUpvalue(WrenVM* vm, ObjUpvalue* upvalue)
{
  // Mark the closed-over object (in case it is closed).
  wrenMarkValue(vm, upvalue->closed);

  // Keep track of how much memory is still in use.
  vm->bytesAllocated += sizeof(ObjUpvalue);
}

void wrenMarkObj(WrenVM* vm, Obj* obj)
{
  if (obj == NULL) return;

  // Stop if the object is already marked so we don't get stuck in a cycle.
  if (obj->marked) return;

  // It's been reached.
  obj->marked = true;

#if WREN_DEBUG_TRACE_MEMORY
  static int indent = 0;
  indent++;
  for (int i = 0; i < indent; i++) printf("  ");
  printf("mark ");
  wrenDumpValue(OBJ_VAL(obj));
  printf(" @ %p\n", obj);
#endif

  // Traverse the object's fields.
  switch (obj->type)
  {
    case OBJ_CLASS:    markClass(   vm, (ObjClass*)   obj); break;
    case OBJ_CLOSURE:  markClosure( vm, (ObjClosure*) obj); break;
    case OBJ_FIBER:    markFiber(   vm, (ObjFiber*)   obj); break;
    case OBJ_FN:       markFn(      vm, (ObjFn*)      obj); break;
    case OBJ_INSTANCE: markInstance(vm, (ObjInstance*)obj); break;
    case OBJ_LIST:     markList(    vm, (ObjList*)    obj); break;
    case OBJ_MAP:      markMap(     vm, (ObjMap*)     obj); break;
    case OBJ_MODULE:   markModule(  vm, (ObjModule*)  obj); break;
    case OBJ_RANGE:    markRange(   vm, (ObjRange*)   obj); break;
    case OBJ_STRING:   markString(  vm, (ObjString*)  obj); break;
    case OBJ_UPVALUE:  markUpvalue( vm, (ObjUpvalue*) obj); break;
  }

#if WREN_DEBUG_TRACE_MEMORY
  indent--;
#endif
}

void wrenMarkValue(WrenVM* vm, Value value)
{
  if (!IS_OBJ(value)) return;
  wrenMarkObj(vm, AS_OBJ(value));
}

void wrenMarkBuffer(WrenVM* vm, ValueBuffer* buffer)
{
  for (int i = 0; i < buffer->count; i++)
  {
    wrenMarkValue(vm, buffer->data[i]);
  }
}

void wrenFreeObj(WrenVM* vm, Obj* obj)
{
#if WREN_DEBUG_TRACE_MEMORY
  printf("free ");
  wrenDumpValue(OBJ_VAL(obj));
  printf(" @ %p\n", obj);
#endif

  switch (obj->type)
  {
    case OBJ_CLASS:
      wrenMethodBufferClear(vm, &((ObjClass*)obj)->methods);
      break;

    case OBJ_FN:
    {
      ObjFn* fn = (ObjFn*)obj;
      DEALLOCATE(vm, fn->constants);
      DEALLOCATE(vm, fn->bytecode);
      DEALLOCATE(vm, fn->debug->name);
      DEALLOCATE(vm, fn->debug->sourceLines);
      DEALLOCATE(vm, fn->debug);
      break;
    }

    case OBJ_LIST:
      wrenValueBufferClear(vm, &((ObjList*)obj)->elements);
      break;

    case OBJ_MAP:
      DEALLOCATE(vm, ((ObjMap*)obj)->entries);
      break;

    case OBJ_MODULE:
      wrenSymbolTableClear(vm, &((ObjModule*)obj)->variableNames);
      wrenValueBufferClear(vm, &((ObjModule*)obj)->variables);
      break;

    case OBJ_STRING:
    case OBJ_CLOSURE:
    case OBJ_FIBER:
    case OBJ_INSTANCE:
    case OBJ_RANGE:
    case OBJ_UPVALUE:
      break;
  }

  DEALLOCATE(vm, obj);
}

ObjClass* wrenGetClass(WrenVM* vm, Value value)
{
  return wrenGetClassInline(vm, value);
}

bool wrenValuesEqual(Value a, Value b)
{
  if (wrenValuesSame(a, b)) return true;

  // If we get here, it's only possible for two heap-allocated immutable objects
  // to be equal.
  if (!IS_OBJ(a) || !IS_OBJ(b)) return false;

  Obj* aObj = AS_OBJ(a);
  Obj* bObj = AS_OBJ(b);

  // Must be the same type.
  if (aObj->type != bObj->type) return false;

  switch (aObj->type)
  {
    case OBJ_RANGE:
    {
      ObjRange* aRange = (ObjRange*)aObj;
      ObjRange* bRange = (ObjRange*)bObj;
      return aRange->from == bRange->from &&
             aRange->to == bRange->to &&
             aRange->isInclusive == bRange->isInclusive;
    }

    case OBJ_STRING:
    {
      ObjString* aString = (ObjString*)aObj;
      ObjString* bString = (ObjString*)bObj;
      return aString->length == bString->length &&
             aString->hash == bString->hash &&
             memcmp(aString->value, bString->value, aString->length) == 0;
    }

    default:
      // All other types are only equal if they are same, which they aren't if
      // we get here.
      return false;
  }
}
