#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "allocator.h"
#include "arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/hash_map.h"
#include "util/list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,

    // One byte: the stride a walk over characters advances by, and the unit a
    // block of them is counted in. Its own kind rather than an int of another
    // width, so that a kind names exactly one type -- and unspellable, since no
    // scope is given the name.
    TYPE_BYTE,

    // An address and nothing more: what a string's characters and an array's
    // elements are reached through. Carries what it points at, so a
    // walk over a block knows its stride without asking the header naming it.
    //
    // Makes no claim beyond the address. It does not own what it names, does
    // not borrow it, and says nothing about how long that memory lives or
    // whether it is live at all -- which is what separates it from both
    // spellings of an indirection. Freeing a block is therefore always the
    // business of the header that knows how many of its elements are live.
    TYPE_PTR,

    // The characters themselves, however many there are. Unsized: no slot holds
    // one, because how far it runs is not in the type. Reached only through a
    // 'ref str', which carries that count beside the address.
    TYPE_STR,

    // A header owning a run of elements: where they are and how many. Its
    // element is whatever it was written over, which is what its drop walk
    // reads.
    TYPE_ARRAY,
    TYPE_STRUCT,

    // An indirection that owns what it names, and one that borrows it. Two
    // kinds rather than one carrying a flag, because whether a slot frees what
    // it names is the whole difference between them, and reading it off the
    // kind is what keeps every other question -- a deref, a field access, a
    // method receiver -- from having to ask about a flag it does not care
    // about. Both carry what they name as their pointee.
    //
    // How wide one is at run time follows from the pointee rather than being
    // stored here, so a pointee that one day needs a length beside its address
    // changes what the constructor computes and nothing else.
    TYPE_BOX,
    TYPE_REF,

    // An owning address with a capacity beside it: a run of elements the value
    // itself frees, however many it was allocated for.
    //
    // A third owning shape rather than a 'box' of many, because the two differ
    // in what says how far the memory runs. A 'box' payload carries an
    // ObjectHeader and is always one element, so nothing has to be counted; a
    // block carries no header, so the capacity rides here and the free is told
    // its size. That is what the raw allocation instructions were reserved for.
    //
    // How many of those elements are live is not its business: a block is
    // capacity, and whatever holds one counts what has been written into it.
    TYPE_BLOCK,
    TYPE_UNKNOWN,
    TYPE_ERROR,
} TypeKind;

// The widest a laid-out type may be. A field's byte offset and an array's
// element offset both ride in an 8-bit instruction operand, so a type wider
// than this has members no instruction could reach.
//
// Stated here rather than read from the instruction encoding, because it is the
// resolver that must reject the type: by the time codegen is picking operands,
// the diagnostic naming the offending declaration is long gone.
#define GAB_MAX_TYPE_BYTES 255

// How many parameters a generic declaration may take. A bound so that
// instantiating one builds its key on the stack; nothing declares more than the
// one 'Vec' takes.
#define GAB_MAX_TYPE_PARAMS 4

// The most steps a supplied drop plan is composed from: what the type's fields
// own, and the live prefix of a block besides. A bound rather than an
// allocation, so composing one fills a local array instead of asking the arena
// twice.
#define GAB_MAX_DROP_STEPS 16

// How many slots a method's signature may take, receiver included. A bound so
// that installing one builds its signature on the stack.
#define GAB_MAX_METHOD_PARAMS 8

/*
    Types compare by pointer identity, so every one a program can name came from
    the registry that interned it. Everything but the registry therefore holds a
    'const Type *': it names an interned type, cannot be used to build one, and
    cannot be written through -- a type is finished when the registry hands it
    over.

    The registry builds with 'Type *' and hands back the const form.
*/
typedef struct Type Type;

/*
    What a reference to a type must carry besides the address.

    A value whose bounds are its own -- an int, a struct, an owning header --
    needs nothing: its type says how wide it is, so an address is the whole
    reference. A run of characters or elements does not: how many there are
    lives with the value rather than with the type, so a reference to one
    carries a count beside the address and is two words instead of one.

    Carried by the pointee rather than by each reference, so that what a 'ref T'
    costs follows from what T is and cannot disagree with it. A slice would
    answer TYPE_META_LENGTH for the same reason characters do, and a value
    reached through a table of methods would answer with that table -- the kind
    of naming a reference carries is what grows here, not the fact that it
    carries one.
*/
/*
    A type constructor, and what it was applied to.

    Every parameterized type in the language is one of these: 'box T', 'ref T',
    'ptr T' and '[T; N]' differ in which constructor and how many arguments,
    not in kind. One interning table keyed by an application therefore serves
    all of them, and serves a generic 'List T' or 'Map K,V' with no new table --
    which is why this is a key rather than one map per constructor.
*/
typedef enum {
    // The built-in constructors, named directly by the compiler.
    TYPE_CTOR_BOX,
    TYPE_CTOR_REF,
    TYPE_CTOR_PTR,
    TYPE_CTOR_BLOCK,

    // A constructor with a declaration behind it: 'Array' today, and whatever
    // a generic declaration introduces later.
    TYPE_CTOR_NOMINAL,
} TypeCtor;

typedef enum {
    // Nothing: the pointee's type says how wide it is.
    TYPE_META_NONE,

    // How many elements the pointee runs to, for a type whose length is not in
    // its type: 'str' today, and a slice when one exists.
    TYPE_META_LENGTH,
} TypeMetadata;

// A method is an ordinary function Symbol — same prototype index, same call
// path — so the map holds one. Declared rather than included: Symbol's own
// header reaches back here, and a method is only ever a function.
typedef struct Symbol Symbol;

typedef struct TypeField {
    String *name;
    const Type *type;
} TypeField;

/*
    A generic declaration: the fields its instantiations are laid out from, in
    terms of the parameters it takes.

    What makes it a declaration rather than a type is that a field may name a
    parameter rather than a type. 'Vec' says its block holds T without saying
    what T is, and interning 'Vec<int>' is that said with int -- which is the
    whole of what instantiating one does.

    Held beside the type the name binds to, so that 'Vec' resolves to something
    a diagnostic can print while naming no value. Nothing is laid out here: a
    width follows from an instantiation's fields, and a parameter has none.
*/
typedef struct TypeParamRef TypeParamRef;

// One field of a declaration, whose type is either fixed or built from the
// parameters. A constructor plus an argument rather than a tree, because what
// a declaration's field may say is exactly this today: 'int', or one of the
// built-in constructors applied to a parameter.
typedef struct GenericField {
    String *name;

    // The type, when it does not mention a parameter.
    const Type *fixed;

    // What the type is built from, when it is not fixed.
    enum {
        // No type at all: what a method returning nothing returns. First, so
        // that a field left zeroed says the thing that cannot be mistaken for
        // an element.
        GENERIC_FROM_NOTHING,

        // One of the declaration's parameters, named by 'param'.
        GENERIC_FROM_PARAM,

        // The instantiation itself, which is what a method's receiver is: a
        // 'Vec<T>' method takes the 'Vec<T>' it was instantiated for, and that
        // type does not exist until the instantiation does.
        GENERIC_FROM_SELF,
    } from;

    // Which parameter, counting from zero. Read only for GENERIC_FROM_PARAM.
    size_t param;

    // What is applied to it, or TYPE_CTOR_NOMINAL to take it as it is.
    TypeCtor ctor;
} GenericField;

/*
    What a generic name declares: how many parameters it takes and what its
    instantiations hold.

    One of these per generic type, built where the builtins are and read where
    an application is interned. A user's own generic declaration would be
    another, which is why this is a shape rather than a special case for 'Vec'.
*/
/*
    One method a generic declaration answers, in terms of its parameters.

    The same shape a field is: a type is either fixed or built from a parameter,
    so what 'push' takes and what 'at' returns are written once and substituted
    per instantiation. A body stays C, since growing a block is what a C body is
    for.
*/
typedef struct GenericMethod {
    // Interned by the provider, as every name the registry is given is.
    String *name;

    // The body, as a GabExternFn. Typed as a void pointer because what a C body
    // is belongs to the VM's header, which this one is reached from rather than
    // reaching.
    void *body;

    // The receiver and the return, each either fixed or from a parameter, and
    // what the caller writes. See GenericField for how one is read.
    GenericField receiver;
    GenericField result;

    const GenericField *params;
    size_t param_count;
} GenericMethod;

typedef struct GenericDecl {
    size_t param_count;

    const GenericField *fields;
    size_t field_count;

    // What every instantiation answers, in terms of the parameters. Registered
    // where an instantiation is interned, since only there is the parameter
    // known -- which is what separates these from an array's shared set.
    const GenericMethod *methods;
    size_t method_count;
} GenericDecl;

// What an indirection names, or NULL for a kind that names nothing. The walks
// asking how many levels deep something is read it that way, so "not an
// indirection" is the answer that stops them rather than a mistake.
// What a type is, and what it is called. A kind is what every reader switches
// on; a name is NULL for a structural type, whose printable form is derived
// rather than stored.
TypeKind type_kind(const Type *type);
String *type_name_of(const Type *type);

// The declaration an instantiation was built from -- the bare 'Array' behind
// every '[T; N]' -- or NULL for a type that is not one.
const Type *type_decl(const Type *type);

// The generic declaration a name introduces, or NULL for every type that is not
// one. What tells 'Vec' apart from 'Vec<int>'.
const GenericDecl *type_generic(const Type *type);

// A list of types nothing in it owns: they belong to the scope arena and outlive
// every compile, so this holds borrowed pointers and frees none.
#define type_list_item_free(item) ((void)(item))
GAB_LIST(TypeList, type_list, const Type *)

const Type *type_pointee(const Type *type);

// The fields a layout was computed from. Empty for a kind laid out some other
// way, so a walk over them is the right no-op there.
const TypeField *type_fields(const Type *type);
size_t type_field_count(const Type *type);

/*
    Building a type, which only the registry does.

    Every type a program can name is owned by the registry that interned it --
    that is what makes pointer identity a sound comparison, and what a
    const Type * asserts. These are declared here because type.c defines them, and
    reaching for one outside the registry is building a type nothing interned.
*/
Type *type_create(Arena *arena, TypeKind kind, String *name);
Type *type_struct_create(Arena *arena, String *name, size_t max_fields);

void type_add_field(Type *type, String *name, const Type *field_type);

// What a reference to this type carries besides the address.
TypeMetadata type_metadata_of(const Type *type);

// Whether this is a reference to characters -- the borrowed way of naming them,
// as an owning 'String' is the other. Its own predicate because the shape is
// two levels deep and asked in several places: what accepts a lend, what a
// receiver reconciles to, what a C body reads, and what '==' and '..' take.
bool type_is_str_ref(const Type *type);

// Whether a value of this type can be held at all: an unsized type names
// something no slot, field or parameter may contain, and is reached only
// through a reference.
bool type_is_sized(const Type *type);

// Whether reaching the value means going through an indirection -- what a
// deref, an auto-deref, and a field access all ask. Says nothing about
// ownership: a 'ref T' is as indirect as a 'box T'.
bool type_is_indirect(const Type *type);

// Whether a value of this type is an address it owns what lies behind -- a
// 'box T' and a 'block T'. Both are freed as themselves rather than through
// parts: a box frees its payload, a block its memory and the live elements in
// it, and neither needs anything kept beside it.
//
// Distinct from type_is_indirect, which asks whether reaching the value means
// going through it. A block is an address and is not dereferenceable: what
// reaches its elements is the header owning it, never a deref.
bool type_owns_through_an_address(const Type *type);

// Whether a value of this type holds the memory it owns in its own slots -- a
// header holding a block, which is what a 'String' and a 'Vec<T>' are.
//
// What it owns therefore lives exactly as long as the slot does, where a
// pointer variable names memory whose lifetime was decided wherever it was
// assigned. That is the difference the flow pass turns on.
bool type_holds_its_memory_inline(const Type *type);

// What one element of an array is, and how many it holds. Both are what the
// application was given, so the element a walk strides by and the count it
// stops at are read from the same place the type was interned on.
const Type *type_array_element(const Type *type);
int32_t type_array_length(const Type *type);

// Whether a value of this type owns memory that must be freed when it dies.
// True of a 'box T' and of an owning string, false of every 'ref', and true of
// a struct exactly when some field of it owns.
//
// Distinct from type_is_indirect because the two questions came apart once a
// string could own: a string owns without being an indirection, and a 'ref T'
// is an indirection that owns nothing.
bool type_is_owned(const Type *type);

// Whether a value of this type duplicates by copying its bytes, which is true
// exactly when nothing it holds transitively owns. See the definition.
bool type_is_copyable(const Type *type);

// Which field a name denotes, or NULL when the type has no such field. Where it
// begins is a layout question, so it is asked of the registry rather than here.
const TypeField *type_find_field(const Type *type, const String *name);

/*
    One part of a lender that a reference to its deref target is built from, as
    a byte offset into the lender and how wide that piece is.

    Offsets rather than fields, because what a reference takes need not be a
    whole field or even a contiguous one: a 'ref str' is an address and a count,
    and a String keeps both inside the block it owns with the capacity between
    them.

    Registered with the deref relation rather than derived, for the same reason
    the relation itself is: nothing about a shape says which of its bytes stand
    for the view it lends. Two types laid out alike lend differently, or not at
    all.
*/
typedef struct TypeRegistry TypeRegistry;

typedef struct LentPart {
    size_t offset;
    size_t size;
} LentPart;

// The most parts a reference is built from: an address and whatever naming its
// pointee requires beside it.
#define GAB_MAX_LENT_PARTS 4

#endif
