//################################################################################
// installed_tree_edit.h
//--------------------------------------------------------------------------------
// The tree-editing subsystem -- right-click-to-edit, category rename,
// delete, create-category, and effect/category drag-and-drop reordering --
// split out of addon.cpp. Six state machines share one shape: Begin*
// populates file-local state and marks it active, Cancel* clears it,
// Render* draws the inline widget and only records a pending job, and
// Apply* consumes that job once the whole tree has finished rendering
// for the frame, so the effect/category arrays are never mutated
// mid-walk. RenderCategoryTree/RenderInstalledEffects (still in
// addon.cpp) reach this subsystem only through the accessor API below,
// never through its statics directly.
//
// The six: category rename, create category, move category
// (reorder-only, via drag-and-drop), effect edit, effect/category
// delete (shared, via the isCategory flag), and effect move
// (drag-and-drop). Only one of the six can be active addon-wide at a
// time -- see AnyEditInFlight -- so an effect edit, say, can never
// overlap a category rename.
//
// Neither category rename nor category move offers reparenting
// (changing a category's parent) -- see the rename group and the move
// group below for why.
//
// Two things below aren't part of that shared shape: single-GUID
// drag-merge (writes straight to disk on drop, no Save click involved --
// see QueueGuidMerge) and the bulk "Delete Empty" sweep (runs inline
// before the tree walk starts, so it doesn't need the
// Queue-then-Apply-after-walk dance the six above rely on).
//--------------------------------------------------------------------------------

#pragma once

#include "core/merge.h" //. nlohmann::ordered_json

#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SplitLines
//--------------------------------------------------------------------------------
// Splits `text` into trimmed, non-empty lines. Used to turn the guids
// text box back into a guid list (effect editor).
//--------------------------------------------------------------------------------
std::vector<std::string> SplitLines(const std::string& text);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// PathHasPrefix
//--------------------------------------------------------------------------------
// True if `prefix` is `path`'s first N elements (N = prefix.size()),
// including equality. Used to tell whether an in-progress edit lives
// at or underneath a category that's about to be collapsed -- see the
// "cancel on collapse" checks in RenderCategoryTree.
//--------------------------------------------------------------------------------
bool PathHasPrefix(const std::vector<int>& path, const std::vector<int>& prefix);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindCategoryByPath
//--------------------------------------------------------------------------------
// Read-only category lookup by index path from `root` (a sin file's
// top-level json, which has its own "categories" array like any other
// category node). Each path element is that level's position within
// its parent's "categories" array at the moment the path was captured
// (see `pathSoFar` in RenderCategoryTree) -- identity is index-based,
// so same-named sibling categories never collide. Returns nullptr if
// any segment is out of range -- callers treat that as "the tree
// changed since editing started, don't guess."
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindCategoryByPath(nlohmann::ordered_json& root, const std::vector<int>& path);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// JoinCategoryPathNames
//--------------------------------------------------------------------------------
// Same walk as FindCategoryByPath, but collects each step's "name"
// for display (result/error messages) instead of returning a pointer,
// joined the same way JoinPath formats a typed destination ("Combat /
// Downstate"). Meant to be called right after a FindCategoryByPath
// lookup on the same path already succeeded; resolves as much as it
// can if the tree changed in between, rather than asserting.
//--------------------------------------------------------------------------------
std::string JoinCategoryPathNames(const nlohmann::ordered_json& root, const std::vector<int>& path);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SetEditResultMessage / ClearEditResultMessage / GetEditResultMessage
//--------------------------------------------------------------------------------
// Shown until the next edit action of any kind succeeds or fails --
// shared by all six state machines (RenderInstalledEffects displays
// it, and every one of them sets it, not just one). Every
// Begin*/Cancel*/Apply* function below calls these instead of touching
// a same-named static directly.
//--------------------------------------------------------------------------------
void SetEditResultMessage(const std::string& message);
void ClearEditResultMessage();
const std::string& GetEditResultMessage();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AnyEditInFlight
//--------------------------------------------------------------------------------
// True if any one of the six state machines -- or the newer bulk
// "Delete Empty" confirm further down -- is currently active, addon-wide
// -- used to grey out/disable starting a different edit while another is
// already open. GUID drag-merge deliberately does NOT feed into this: it
// only ever runs while its source effect's own edit is already the thing
// holding AnyEditInFlight true, so it needs no separate flag.
//--------------------------------------------------------------------------------
bool AnyEditInFlight();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginCategoryEdit / CancelCategoryEdit / RenderCategoryEditor / ApplyPendingCategoryRename
//--------------------------------------------------------------------------------
// Category editing -- name and description, same "description" key as
// effects, still a smaller sibling of the effect editor further down:
// reparenting isn't offered here, or by category move below, since it
// would silently carry every effect/subcategory underneath along for
// the ride, and nothing's asked for that yet. `path` is this
// category's own identity, root -> ... -> this category, inclusive.
// Render only records the pending job; Apply re-finds the category
// by path and writes it, safe to call unconditionally every frame.
//--------------------------------------------------------------------------------
void BeginCategoryEdit(const std::string& sinName, const std::vector<int>& path,
                       const std::string& currentName, const std::string& currentDescription);
void CancelCategoryEdit();
void RenderCategoryEditor();
void ApplyPendingCategoryRename();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IsCategoryBeingRenamed / IsCategoryRenameUnderPath / IsCategoryRenameActive
//--------------------------------------------------------------------------------
// True for the category at exactly this path, for a rename anywhere
// at or underneath this path (cancels it when the owning node
// collapses), and for a rename active anywhere (any sin file) --
// respectively. The last feeds AnyEditInFlight above.
//--------------------------------------------------------------------------------
bool IsCategoryBeingRenamed(const std::string& sinName, const std::vector<int>& path);
bool IsCategoryRenameUnderPath(const std::string& sinName, const std::vector<int>& path);
bool IsCategoryRenameActive();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginCreateCategory / CancelCreateCategory / RenderCreateCategoryEditor / ApplyPendingCreateCategory
//--------------------------------------------------------------------------------
// Rendered inside the parent category's TreeNode (same idea as
// category rename above), so -- unlike a delete confirmation -- this
// DOES get cancelled on collapse. `parentPath` is where the new
// category goes; empty means this sin file's top level. Render only
// records the pending creation; Apply re-finds the parent by path and
// writes it, safe to call unconditionally every frame.
//--------------------------------------------------------------------------------
void BeginCreateCategory(const std::string& sinName, const std::vector<int>& parentPath);
void CancelCreateCategory();
void RenderCreateCategoryEditor();
void ApplyPendingCreateCategory();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IsCreatingCategoryAt / IsCategoryCreateUnderPath / IsCreateCategoryActive
//--------------------------------------------------------------------------------
// True for a create-category prompt targeting exactly this parent
// path, for one targeting at or underneath this path (cancels it on
// collapse, same as category rename), and for one open anywhere --
// respectively. The last feeds AnyEditInFlight.
//--------------------------------------------------------------------------------
bool IsCreatingCategoryAt(const std::string& sinName, const std::vector<int>& parentPath);
bool IsCategoryCreateUnderPath(const std::string& sinName, const std::vector<int>& path);
bool IsCreateCategoryActive();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginCategoryDrag / GetCategoryDragSinName / GetCategoryDragPath
//--------------------------------------------------------------------------------
// Records that the category at `path` (in `sinName`) is the one
// currently being dragged, and hands ImGui the drag payload -- call
// from inside ImGui::BeginDragDropSource(). The category's own
// identity is `path` itself (root -> ... -> this category, inclusive);
// path.back() is its index within its parent's "categories" array.
// The two getters return whatever BeginCategoryDrag most recently
// recorded this frame -- a drop target reads them to decide whether
// (and how) to queue a move. An empty path means nothing is currently
// being dragged.
//--------------------------------------------------------------------------------
void BeginCategoryDrag(const std::string& sinName, const std::vector<int>& path);
const std::string& GetCategoryDragSinName();
const std::vector<int>& GetCategoryDragPath();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// QueueCategoryMove / ApplyPendingCategoryMove
//--------------------------------------------------------------------------------
// Reorder-only: a category only ever lands back among its own current
// siblings, at a new position -- reparenting isn't offered, same
// reasoning as category rename above. `originalPath` is the dragged
// category's identity at drag time; `destinationIndex` is -1 for
// "append" (dropped on the shared parent's own row) or the sibling
// index -- within that parent's "categories" array as captured at
// drop time, before any erase has run -- to land immediately above.
// Apply re-finds the array by path/index and writes it, safe to call
// unconditionally every frame.
//--------------------------------------------------------------------------------
void QueueCategoryMove(const std::string& sinName, const std::vector<int>& originalPath, int destinationIndex);
void ApplyPendingCategoryMove();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginEdit / CancelEdit / RenderEffectEditor / ApplyPendingEdit
//--------------------------------------------------------------------------------
// Lets a user pick one effect and makes its own fields -- name,
// description, guids -- editable. Behaviors (Hide/Show/SetDuration)
// are never made editable here; those stay owned by VfxDenoiser's own
// UI, rendered read-only regardless of edit state. Only one edit can
// be in flight at a time, addon-wide -- see AnyEditInFlight. `path`/
// `index` are the effect's containing-category identity and its
// position within that category's "effects" array; `effect` seeds the
// edit buffers. Render only records the pending save; Apply re-finds
// the effect by path/index and writes it, safe to call unconditionally
// every frame.
//--------------------------------------------------------------------------------
void BeginEdit(const std::string& sinName, const std::vector<int>& path,
                int index, const nlohmann::ordered_json& effect);
void CancelEdit();
void RenderEffectEditor();
void ApplyPendingEdit();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IsEffectBeingEdited / IsEffectEditUnderPath / IsEffectEditActive
//--------------------------------------------------------------------------------
// True for the effect at exactly (sinName, path, index), for an edit
// anywhere at or underneath `path` (cancels it on collapse), and for
// an edit active anywhere -- respectively. The last feeds
// AnyEditInFlight.
//--------------------------------------------------------------------------------
bool IsEffectBeingEdited(const std::string& sinName, const std::vector<int>& path, int index);
bool IsEffectEditUnderPath(const std::string& sinName, const std::vector<int>& path);
bool IsEffectEditActive();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginDeleteConfirm / CancelDeleteConfirm / RenderDeleteConfirm / ApplyPendingDelete
//--------------------------------------------------------------------------------
// Shared by effects and categories via `isCategory` -- a category
// delete also re-checks it's still empty at apply time. `path` is the
// effect's containing category (effect delete) or the category's own
// identity, inclusive (category delete); `index` is the effect's
// position within that "effects" array, unused for a category.
// Rendered inline next to the item's own row, which stays visible
// whether or not its TreeNode is open -- unlike the editors above,
// this deliberately does NOT cancel on collapse. Apply re-finds the
// target by path/index, safe to call unconditionally every frame.
//--------------------------------------------------------------------------------
void BeginDeleteConfirm(const std::string& sinName, const std::vector<int>& path, int index,
                         bool isCategory, const std::string& displayName);
void CancelDeleteConfirm();
void RenderDeleteConfirm();
void ApplyPendingDelete();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IsDeletingThisCategory / IsDeletingThisEffect / IsDeleteConfirmActive / IsDeleteConfirmUnderPath
//--------------------------------------------------------------------------------
// True for the pending confirmation matching exactly this category, or
// this effect (containing-category path + index), or one active
// anywhere (feeds AnyEditInFlight) -- respectively. IsDeleteConfirm-
// UnderPath is true if the pending confirmation (either kind) is at or
// underneath `path`; unlike the other UnderPath checks it's NOT used
// to cancel on collapse (the confirm row stays visible collapsed or
// not) -- it's needed by tree-search hiding instead, where the whole
// row disappears.
//--------------------------------------------------------------------------------
bool IsDeletingThisCategory(const std::string& sinName, const std::vector<int>& path);
bool IsDeletingThisEffect(const std::string& sinName, const std::vector<int>& path, int index);
bool IsDeleteConfirmActive();
bool IsDeleteConfirmUnderPath(const std::string& sinName, const std::vector<int>& path);

//_ Payload marker for ImGui::SetDragDropPayload/AcceptDragDropPayload on
// an effect drag -- bytes are just a type marker, never read back; real
// source info travels via GetEffectDragPayload (see EffectDragPayload).
inline constexpr int kEffectDragMarker = 1;

//********************************************************************************
// EffectDragPayload
//--------------------------------------------------------------------------------
// sinName         which sin file the dragged effect belongs to
// originalPath    containing category's identity at drag time
// effectName      display name, used for messages
// originalIndex   position within originalPath's "effects" array --
//                 the real identity, since sibling effects can share
//                 a name
//--------------------------------------------------------------------------------
// Exposed as a struct (rather than one getter per field) since
// RenderCategoryTree's drop-target logic reads several fields
// together for same-category/no-op comparisons.
//--------------------------------------------------------------------------------
struct EffectDragPayload
{
    std::string      sinName;
    std::vector<int> originalPath;
    std::string      effectName;
    int              originalIndex = -1;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginEffectDrag / GetEffectDragPayload
//--------------------------------------------------------------------------------
// Records the current drag payload -- call every frame the drag is
// held, from inside ImGui::BeginDragDropSource(). GetEffectDragPayload
// is only meaningful while a drag is in progress (i.e. from inside a
// BeginDragDropTarget block that just accepted "VFXD_EFFECT").
//--------------------------------------------------------------------------------
void BeginEffectDrag(const std::string& sinName, const std::vector<int>& path,
                      const std::string& effectName, int index);
const EffectDragPayload& GetEffectDragPayload();

//********************************************************************************
// EffectMoveJob
//--------------------------------------------------------------------------------
// sinName             sin file the move applies to
// originalPath        containing category's identity at drag time
// effectName          display name, for messages
// originalIndex       position within originalPath's "effects" array
// destinationPath     drop target category's identity
// destinationIndex    -1 for "append" (dropped on the category's own
//                     row), else the sibling index -- within the
//                     destination "effects" array as captured at drop
//                     time, before any erase runs -- to land
//                     immediately above; see ApplyPendingMove for the
//                     erase-shift reconciliation when
//                     originalPath == destinationPath
//--------------------------------------------------------------------------------
struct EffectMoveJob
{
    std::string      sinName;
    std::vector<int> originalPath;
    std::string      effectName;
    int              originalIndex = -1;
    std::vector<int> destinationPath;
    int              destinationIndex = -1;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// QueueEffectMove / ApplyPendingMove
//--------------------------------------------------------------------------------
// Queue records a move to apply once the whole tree has finished
// rendering this frame -- called from both of RenderCategoryTree's
// effect drop targets (the category-row "append" target and the
// specific-effect-row "insert above" target). Apply re-finds the
// source and destination categories by path and writes the move, safe
// to call unconditionally every frame.
//--------------------------------------------------------------------------------
void QueueEffectMove(EffectMoveJob job);
void ApplyPendingMove();

//_ Payload marker for a single-GUID drag -- same "bytes are just a type
// marker" convention as kEffectDragMarker above.
inline constexpr int kGuidDragMarker = 1;

//********************************************************************************
// GuidDragPayload
//--------------------------------------------------------------------------------
// sinName         which sin file the dragged GUID's effect belongs to
// originalPath    that effect's containing category's identity
// originalIndex   that effect's position within originalPath's
//                 "effects" array -- together with originalPath, the
//                 source effect's real identity
// effectName      source effect's display name, used for messages
// guid            the actual GUID string being moved
//--------------------------------------------------------------------------------
struct GuidDragPayload
{
    std::string       sinName;
    std::vector<int>  originalPath;
    int               originalIndex = -1;
    std::string       effectName;
    std::string       guid;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginGuidDrag / GetGuidDragPayload
//--------------------------------------------------------------------------------
// Records the current single-GUID drag payload -- call every frame the
// drag is held, from inside a GUID bullet's own BeginDragDropSource in
// installed_tree_view.cpp's RenderGuidList (GuidListDragContext branch).
// Deliberately not offered from inside the effect editor itself -- see
// QueueGuidMerge for why. GetGuidDragPayload is only meaningful from
// inside a BeginDragDropTarget block that just accepted "VFXD_GUID".
//--------------------------------------------------------------------------------
void BeginGuidDrag(const std::string& sinName, const std::vector<int>& path,
                    int index, const std::string& effectName, const std::string& guid);
const GuidDragPayload& GetGuidDragPayload();

//********************************************************************************
// GuidMergeJob
//--------------------------------------------------------------------------------
// sinName                  sin file the merge applies to
// originalPath/Index       source effect's identity (see GuidDragPayload)
// effectName               source effect's display name, for messages
// guid                     the GUID being moved
// destinationPath/Index    target effect's identity, captured at drop
//                          time -- any effect row in the same sin file,
//                          open or collapsed, is a valid target
// destinationEffectName    target effect's display name, for messages
//--------------------------------------------------------------------------------
struct GuidMergeJob
{
    std::string       sinName;
    std::vector<int>  originalPath;
    int               originalIndex = -1;
    std::string       effectName;
    std::string       guid;
    std::vector<int>  destinationPath;
    int               destinationIndex = -1;
    std::string       destinationEffectName;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// QueueGuidMerge / ApplyPendingGuidMerge
//--------------------------------------------------------------------------------
// Queue records a same-sin GUID move-and-merge to apply once the whole
// tree has finished rendering this frame -- called from the destination
// effect row's drop target, right alongside its VFXD_EFFECT accept.
// Writes straight to disk on both ends, no Save click needed: the
// source is always a plain read-only bullet, never the effect actually
// open for editing, and the destination row refuses the drop while it's
// the one being edited (its GUIDs textbox is a stale BeginEdit-time
// snapshot until Save overwrites it wholesale). Skips the add on the
// destination side if it's already present there, so a merge never
// creates a cross-effect duplicate.
//--------------------------------------------------------------------------------
void QueueGuidMerge(GuidMergeJob job);
void ApplyPendingGuidMerge();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CountEmptyGuidEffects
//--------------------------------------------------------------------------------
// Number of effects, across every currently loaded sin file, whose
// "guids" array is missing or empty -- an effect a GUID drag-merge (or
// a manual delete, once saved) has hollowed out. Used both to grey out
// "Delete Empty" when there's nothing to do and to word its confirmation.
//--------------------------------------------------------------------------------
int CountEmptyGuidEffects();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginDeleteEmptyConfirm / CancelDeleteEmptyConfirm / RenderDeleteEmptyConfirm / IsDeleteEmptyConfirmActive
//--------------------------------------------------------------------------------
// A bulk sibling of BeginDeleteConfirm above: sweeps every empty effect
// in every loaded sin file in one go, rather than one at a time. Render
// re-counts (in case something changed since Begin) and, unlike the
// single-effect confirm, performs the sweep itself the moment it's
// confirmed -- it's called from RenderInstalledEffects before the tree
// walk starts, so (unlike every job above) there's no mid-walk hazard in
// mutating the tree right there instead of deferring it.
//--------------------------------------------------------------------------------
bool IsDeleteEmptyConfirmActive();
void BeginDeleteEmptyConfirm();
void CancelDeleteEmptyConfirm();
void RenderDeleteEmptyConfirm();