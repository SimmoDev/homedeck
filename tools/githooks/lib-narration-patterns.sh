#!/usr/bin/env bash
# Shared narration-pattern list for check-docs.sh (doc/code files) and
# commit-msg (commit messages) - sourced by both rather than duplicated,
# so a new pattern only needs adding in one place. Not itself a runnable
# hook.
#
# Deliberately does NOT flag every "reference unit"/"hardware" mention -
# hardware.md has its own documented, intentional convention of marking
# facts **Confirmed** against the physical unit vs. spec-sheet claims (see
# its own opening paragraph), which is real content, not narration. Patterns
# below require an activity verb (confirmed/ruled out/observed/verified/
# reliable/etc.) actually adjacent to hardware/reference-unit wording, not
# bare co-occurrence, to keep the false-positive rate low enough to stay
# useful.
narration_patterns=(
    # Covers "confirmed on hardware", "confirmed on real hardware",
    # "confirmed on the K145 reference unit", "confirmed against the
    # project's own reference unit", "confirmed running live on the Tab5
    # K145 reference unit" - the '.{0,80}' middle section (rather than a
    # strict alnum character class) deliberately tolerates apostrophes,
    # extra adjectives, and other punctuation between the verb and its
    # object so phrasing variants aren't missed the way a narrower
    # character class would miss them.
    'confirmed .{0,80}(hardware|reference unit)'
    'confirmed end.to.end'
    'confirmed manually'
    'confirmed via'
    'verified working'
    'no regression observed'
    'ruled out every'
    'reliable across repeated attempts'
    'work(s|ing)? .{0,40}reference unit'
    'earlier attempt'
    'retried [0-9]+/[0-9]+'
    # Verification-log summary phrasing - counting passes/rounds or
    # tallying found-vs-fixed issues describes the review process itself
    # rather than stating what changed, the same class of narration as
    # the hardware-verification patterns above, just for review/testing
    # activity instead of hardware bring-up. Caught in a commit message
    # closing M2 that a human reviewer flagged as narration despite not
    # matching any pattern above - added to close that gap.
    '[0-9]+ (passes|rounds) (of|total)'
    '(found|fixed) [0-9]+ (issue|bug|finding)'
    'zero (blocker|high|medium|low)'
    '(re-)?verified (after|following) (the|every|each) fix'
)
