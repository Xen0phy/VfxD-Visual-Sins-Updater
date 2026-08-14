# Standalone replica of sql_update.cpp's filtering/state-transition rules,
# checked against synthetic data since no real seeded db was provided this
# pass. Not a substitute for running it against VfxD_Greed.sqlite3.

def count_emitted(effects, variant):
    n = 0
    for e in effects:
        if not e["categoryPath"]:
            continue
        if variant == "Sloth" and e["categoryPath"][0] == "Caution":
            continue
        n += 1
    return n

def state_for(installed_version, latest_version, is_installed):
    if not is_installed:
        return "NotInstalled"
    return "UpdateAvailable" if installed_version < latest_version else "UpToDate"

effects = [
    {"categoryPath": ["Skill Effects"]},
    {"categoryPath": ["Skill Effects", "Combos"]},
    {"categoryPath": []},                      # uncategorized -> excluded from all variants
    {"categoryPath": ["Caution"]},              # excluded from Sloth only
    {"categoryPath": ["Caution", "Sub"]},       # excluded from Sloth only (top-level check)
]

gluttony = count_emitted(effects, "Gluttony")
pride    = count_emitted(effects, "Pride")
sloth    = count_emitted(effects, "Sloth")

assert gluttony == 4, gluttony   # everything but the uncategorized one
assert pride == 4, pride         # Pride doesn't drop Caution
assert sloth == 2, sloth         # Sloth also drops both Caution rows

# state transitions
assert state_for(-1, 10, False) == "NotInstalled"
assert state_for(5, 10, True) == "UpdateAvailable"
assert state_for(10, 10, True) == "UpToDate"
assert state_for(12, 10, True) == "UpToDate"   # SQL count lower than installed -> UpToDate, never a downgrade

print("All synthetic checks passed.")
