# Proposed §30. Field reflection — reading another mod's data without linking to it

**Status: not built.** Written as a spec so it can be implemented and then folded
into `FORK_CHANGES.md` as §30 in the same shape as §22–§29.

---

## The problem

A mod that wants to *describe* another mod's weapon cannot read it.

ZScript can only touch a field through a typed reference, and a typed reference
needs the class **at compile time**:

```zscript
let w = DRLA_PlasmaRifle(weap);   // needs DRLA loaded to COMPILE
int dmg = w.DamageBase;           // ...or this file does not build at all
```

That is a hard dependency, and it is the wrong shape for an informational
consumer. A weapon-select menu that shows tier, rarity, affixes and damage wants
to work with DoomRL Arsenal, LegenDoom, Doomablo and mods not written yet — none
of which will ever declare a compatibility interface for it, and none of which
it can afford to require.

The existing escape hatch is `Service` (`service.zs`): a mod publishes a
subclass, a consumer reaches it via `ServiceIterator.Find(String)` and asks
questions by string. That works and this fork's consumers already use it — but
it only works **when the other mod opts in**. DRLA will not. LegenDoom will not.
Nothing already released will.

## The observation

The VM already knows everything required. Every field of every class, in every
loaded mod, is described at runtime:

- `PClass::Fields` — `TArray<PField *>`, every field on the class
  (`src/common/objects/dobjtype.h:83`)
- `PClass::FindSymbol(FName, bool searchparents)` — name lookup, optionally
  walking base classes (`dobjtype.h:50`)
- `PField::Offset`, `PField::Type`, `PField::Flags`
  (`src/common/scripting/core/symbols.h:78-94`)

The data is complete and correct. It is simply not exposed to script. Nothing
here invents a new mechanism; it opens a door onto one the VM maintains anyway.

---

## The natives

All addressed by **string**, so a caller resolves at runtime and carries no
compile-time reference to anything.

### Typed reads

```
bool Level.HasField(Object o, string field)
bool Level.GetFieldInt(Object o, string field, out int value)
bool Level.GetFieldFloat(Object o, string field, out double value)
bool Level.GetFieldString(Object o, string field, out string value)
bool Level.GetFieldName(Object o, string field, out Name value)
bool Level.GetFieldObject(Object o, string field, out Object value)
```

Each returns **false** and leaves `value` untouched when the field does not
exist, is the wrong type, or is not readable (below). False is "I could not
answer", never "the answer is zero" — a caller must be able to tell an absent
stat from a stat that is genuinely 0, because on a data sheet those two render
differently and conflating them is how a panel starts lying.

`Object`, not `Actor`. Weapon data is frequently held on a non-Actor helper
object hanging off the weapon, and restricting the entry point to Actor would
put that out of reach for no gain.

### Enumeration

```
int  Level.FieldCount(Object o)
bool Level.FieldAt(Object o, int index, out string name, out string type)
```

**This is the half that matters.** Typed reads let a consumer ask a question it
already knew to ask; enumeration lets it discover what there is to ask. Point it
at a weapon from a mod nobody has written an adapter for and it can list what
that weapon actually carries — which is the difference between a menu that
supports a fixed list of mods and one that degrades usefully on all of them.

`type` is returned as a plain string (`"int"`, `"double"`, `"string"`,
`"name"`, `"object"`, `"other"`) so a caller can pick the right getter without
the fork having to export the type system.

---

## Rules the implementation must hold

### Type-check before reading, always

`Offset` is a byte offset into the object. Reading an `int32` field through a
`double*` is not a wrong answer, it is garbage or a crash. Every getter
**must** compare `field->Type` against the expected singleton
(`TypeSInt32`, `TypeUInt32`, `TypeFloat32`, `TypeFloat64`, `TypeString`, …
declared at `src/common/scripting/core/types.h:725-728`) and return false on
mismatch rather than reinterpreting the bytes.

Widening is the one permitted courtesy: `GetFieldFloat` may serve a `TypeFloat32`
or an integer field, because every value survives the conversion. Narrowing must
not be — a `double` read as `int` silently discards, and a stat sheet quietly
showing 3 for 3.7 is worse than showing nothing.

### Read-only, permanently

There is deliberately no `SetField`. Writing into another mod's private state
turns every subsequent crash into one nobody can attribute — the corruption and
the symptom would be in different mods, and the mod that did it leaves no trace.
Reading cannot corrupt anything. Keep the asymmetry.

### Which fields are visible

Refuse:

- `VARF_Private` (`types.h:20`) — the declaring mod said no
- `VARF_Meta` (`types.h:33`) — class data, not instance data; reading it at an
  instance offset is meaningless
- `VARF_Static` (`types.h:28`) — same reason

Allow `VARF_ReadOnly` (`types.h:19`): read-only to script means it may not be
written, and this only ever reads. `VARF_Deprecated` (`types.h:22`) should read
fine and is worth reporting through `FieldAt`'s `type` string or a flag, so a
consumer can prefer a live field where both exist.

`FindSymbol` is called with `searchparents = true`, so a subclass reports
inherited fields — otherwise every consumer would have to know a mod's class
hierarchy to find `Weapon`'s own members, which is exactly the knowledge this
is meant to remove the need for.

### Null and bounds

Null object returns false / count 0. `FieldAt` with an out-of-range index
returns false. Neither is an error worth a console line: enumeration loops are
expected to probe.

---

## What this does not solve

A value that was never stored cannot be read, because there is nothing to read.

```zscript
A_FireBullets(0, 0, 1, 25);     // 25 is an operand in compiled bytecode
```

There is no field holding 25. `FState` does carry `ActionFunc`
(`src/gamedata/info.h:104`) but it is deliberately not among the fields exported
to script in `p_states.cpp:1136-1150`, and even exported it would be a
`VMFunction*` — the arguments are pushed on the VM stack at call time and are
not retained as data anywhere.

This matters less than it sounds. Any mod that *rolls* stats per instance —
which is every loot mod, and the entire category this is for — must store them
in fields to vary them at all. Weapons with hardcoded damage are the ones whose
damage never varies, so a consumer can read them once from `Default` or measure
them by observation.

---

## Suggested implementation

Mirrors §24–§26: a small resolver plus one thunk per getter.

```cpp
static PField *ResolveField(DObject *o, const FString &name)
{
    if (o == nullptr) return nullptr;
    PSymbol *sym = o->GetClass()->FindSymbol(FName(name.GetChars()), true);
    PField *f = dyn_cast<PField>(sym);
    if (f == nullptr) return nullptr;
    if (f->Flags & (VARF_Private | VARF_Meta | VARF_Static)) return nullptr;
    return f;
}
```

then per getter, e.g.

```cpp
DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldInt)
{
    PARAM_PROLOGUE;
    PARAM_OBJECT(o, DObject);
    PARAM_STRING(name);
    PARAM_OUTPOINTER(out, int);

    PField *f = ResolveField(o, name);
    if (f == nullptr || (f->Type != TypeSInt32 && f->Type != TypeUInt32))
        ACTION_RETURN_BOOL(false);

    if (out) *out = *(int *)((uint8_t *)o + f->Offset);
    ACTION_RETURN_BOOL(true);
}
```

### Touched

- `src/scripting/vmthunks.cpp` — the resolver and the eight thunks
- `wadsrc/static/zscript/doombase.zs` — the native declarations on `LevelLocals`

**Both build targets**, as §25 and §26 note: the executable *and* the pk3 target
carrying the ZScript declarations. Building only the first gives a clean compile
and a script error at load.

---

## What it costs

A `FindSymbol` per call — a name hash against the class's symbol table. Fine per
selection change, not per tic per card. A consumer polling forty weapons every
frame should cache; that is the consumer's problem and worth stating in the doc
rather than solving with an engine-side cache that would then need invalidating.

## What it buys

One consumer describing weapons from every loaded mod, present and future,
with no compile-time reference to any of them and no cooperation required from
their authors — including mods released after both this fork and that consumer
stop being maintained.
