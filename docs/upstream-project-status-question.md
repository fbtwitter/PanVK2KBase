# Upstream question: is a kbase backend something Panfrost would take?

Different in kind from the other two files in this directory. Those ask
about one specific design decision each — the ringbuf's double mapping, the
dma-buf import hook — and are scoped so a "yes" or "no" to either doesn't
commit anyone to anything about the project as a whole. This one asks the
question neither of them answers: **is this worth taking upstream at all,
or is it better as a maintained fork?**

Worth sending separately, and worth being clear about why it wasn't asked
first. `docs/upstream-ringbuf-question.md`'s own status note already flags
this — the plan was "raise the project before Phase 4", and what actually
happened was the opposite: two narrow technical questions went out once
there was something concrete to ask about, and the broader framing never
got made. That's not a mistake to fix retroactively so much as a gap worth
closing now that there is a real status to report instead of a plan.

Status: **drafted, not sent.** Written 2026-08-01.

Send this after, not before, the other two — it reads better as "here's
where things stand, including two open questions already in flight" than
as a cold introduction, and a maintainer who has already seen one of the
other two arrive will have context for it.

---

## The message

```
Wider question than my last two here — those were both "is this one design
decision okay", this one is "should this exist upstream at all".

I've been porting PanVK to kbase, Arm's legacy out-of-tree Mali kernel
driver, on a Mali-G720 (Poco X8 Pro), as an out-of-tree experiment:
github.com/fbtwitter/PanVK2KBase.

Why: essentially every shipping Android phone with a Mali GPU runs kbase,
not panthor or panfrost, so PanVK cannot run on real Android hardware today
regardless of how conformant it gets. Same shape of problem Turnip solved
for Adreno via its kgsl backend, which is what eventually made Turnip
loadable as a standalone driver in the custom-driver pickers Android
enthusiasts already use (Eden, Azahar, Winlator, etc.) - I'm aiming at the
same outcome for Mali, not at replacing panthor.

Where it actually is: compute works end to end on real hardware - a
pipeline built from application SPIR-V, vkCmdDispatch, a VkFence signalled
by the GPU, correct results read back, stable across 2000 back-to-back
submits. Binary and timeline semaphores work. The tiler heap and the
kbase-specific queue/group integration are both done. Rendering is blocked
on one thing, which is the subject of my other message here - the render
descriptor ringbuf's double mapping doesn't have an equivalent on kbase.

I know kbase support isn't a stated priority - Panthor and now Tyr are
where the energy is, for good reasons I'm not contesting. So the actual
question is: is a kbase backend something that would be considered for
Mesa at all, gated behind something like an env var (PAN_USE_KRAID is the
precedent I had in mind) and landed as small reviewable MRs per phase - or
would you rather this stay a maintained out-of-tree fork indefinitely?

I'd rather find that out now than build toward an integration nobody wants
merged. Either answer is useful to me; I just want to stop guessing which
one it is.

No urgency on this relative to the other two questions - those are
blocking actual work, this one is closer to "before I sink more time into
the packaging/WSI phases, is this headed anywhere upstream."
```

---

## What "no" would mean, for the record

If the answer is "this stays a fork," `docs/architecture.md` and this
project's `README.md` already describe it as an out-of-tree experiment, so
nothing about the current framing needs to change. The main practical
consequence is scope: features like dma-buf import (the subject of
`upstream-import-question.md`) would need a fork-maintained patch to
`pan_kmod.c` rather than an upstream change, which is more maintenance
burden per Mesa version bump but not a blocker to anything already working.

## What this is not asking

Not asking for review of the code, not asking anyone to test it, not
asking for a timeline. Just the one question: accepted-in-principle, or
not. Everything else in this repo's `docs/upstream-*` files stands on its
own regardless of the answer.
