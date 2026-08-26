# Moiren — control application design brief

A design brief for the desktop control application. It describes **what the
application is for, what it must show, and what it must never get wrong**. It
is written to be handed to a designer or a design tool as the sole source of
context: everything needed to lay out the interface is here.

Back-end plumbing is a separate job. Where a number comes from is noted so the
wiring is obvious later, but nothing here depends on it existing yet.

---

## 1. What this machine is

**Moiren** is a selective laser melting (SLM) metal 3D printer. It builds parts
by spreading a 30 µm layer of metal powder and melting a cross-section into it
with a 500 W fibre laser, then repeating — **1,500 to 1,700 times** for a
typical part. A print runs for many hours, often overnight.

The chamber is flooded with argon. Powder plus oxygen plus a laser is a fire;
the argon purge is what stops that, and it takes up to forty minutes before a
print can begin.

Three boards run the machine:

| Board | Owns |
|---|---|
| **Teensy 4.1** (galvo card) | the laser, the mirrors that steer it, the job file on its SD card |
| **Arduino Mega** | the three axes, oxygen and temperature sensing, the interlock chain, argon and airflow, chamber lighting |
| **ESP32** | airflow, eventually. Not built yet — do not design for it. |

### The single most important thing about this application

**The machine does not need it.** The printer runs a job to completion with the
computer switched off or unplugged. The application adds a user interface, job
upload and camera monitoring — nothing a print depends on.

This is a deliberate machine-design decision and it shapes the UI: the app is an
**observer and a commander, never a participant**. It must never present itself
as the thing keeping the print alive, never block on its own connection, and
must reconnect mid-print and pick up an accurate picture from the boards
themselves.

### Who uses it

One person, in a workshop, who built this machine. They are at a desk beside it,
or on a laptop checking on an overnight run. They are not a trained operator
following a procedure — they are the engineer, and they will be reading this
interface to work out why something went wrong.

Two moods to serve:

- **Watching** — a print is running, hours to go. Glanceable. Is it fine?
- **Working** — setting up, troubleshooting, running maintenance. Dense,
  precise, everything reachable.

---

## 2. Design principles

**Honesty over reassurance.** This machine can start a fire, destroy a
£-thousands laser head, and waste days of build time. Every place the interface
could show something more comfortable than the truth, it must show the truth.
Specific cases in §7 — they are not decoration, each exists because the
alternative caused a real failure.

**Distinguish "not known" from "fine".** A sensor that is not reporting is not a
sensor reading zero. An axis whose position was never established is not an axis
at zero. Blank, greyed or explicitly "unknown" — never a plausible number.

**Refusal is normal, not an error.** Large parts of the machine are not built
yet, and the firmware answers `REFUSED` rather than pretending. The UI should
show those controls as unavailable, not offer them and surface a scary error.

**Long operations get progress, not spinners.** A purge is forty minutes. A job
upload is two minutes. A print is fifteen hours. Show the thing that is actually
changing — oxygen falling toward its target, megabytes transferred, layers
completed — never an indeterminate animation.

**Nothing destructive without a deliberate act.** E-stop, aborting a print, and
moving an axis past its travel limits each need real intent. But E-stop must be
reachable **instantly, from every screen, always**.

---

## 3. Visual direction

A workshop instrument, not a consumer app. Precise, calm, legible from a metre
away when it matters.

- **Dark theme as the default.** The machine sits in a workshop and is watched at
  night. A light theme is welcome but secondary.
- **Numbers are the hero.** Large tabular figures with their units. Never make
  someone squint at a temperature.
- **Colour means state, and only state.** Reserve it: green for verified-good,
  amber for a warning band, red for tripped or overridden, grey for unknown.
  Do not decorate with colour that carries no meaning.
- **A monospace face for anything numeric or logged**, so digits do not jump as
  they change.
- **Space is affordable.** The application runs full-screen on a desktop, target
  1600×1000 and up. Do not cram.

The existing prototype is a Qt application with a left sidebar of stat cards and
a dense main panel. That basic shape works and is familiar to its user — start
there rather than reinventing it.

---

## 4. Screen inventory

| Screen | Purpose |
|---|---|
| **Dashboard** | The default. Everything you need while a print runs. |
| **Manual control** | Jogging, homing, recoat, lights, gas — setup and troubleshooting. |
| **Job** | Choose, convert, upload and start a print. |
| **Sensors** | Full detail and history for every channel. |
| **Camera** | Live view and the per-layer image record. |
| **Settings** | Machine values that get dialled in once. |
| **Calibration** | Field correction table, timing offset, travel measurements. |
| **Log** | Everything the boards have said. |

A persistent **status bar** and an always-visible **E-STOP** frame all of them.

---

## 5. Persistent frame

### E-STOP

Present on every screen, unmissable, never scrolled away. Red, large, top-right
or a fixed corner.

Pressing it needs no confirmation — that is the point. It **latches**: the
machine stays stopped, and it can only be cleared by physically resetting the
boards. Say so on screen when latched, because someone will look for a software
way out and there isn't one.

When latched, the whole interface should visibly change state — a red border,
controls disabled — so it is obvious at a glance from across the room.

### Status bar

Always visible, one line:

- **Machine state** — Idle · Homing · Purging · Ready · Printing · Paused ·
  Fault · E-stopped
- **Connection**, per board — the Mega and the Teensy connect independently and
  either can be missing. "Connected" is not one lamp.
- **Job**, when one is running — layer 412 / 1668, elapsed, estimated remaining
- **Alert count**, if any, clicking through to the list

**Connection lost is not an emergency.** Show it plainly, keep displaying the
last known values clearly marked as stale, and reconnect quietly. The print is
still going.

---

## 6. Dashboard

The screen that is up for fifteen hours. It answers, in order: *is it safe*,
*is it working*, *how far along*.

### Left rail — vital signs

Big stat cards, glanceable:

- **Oxygen** — the number that matters most. Percent, one decimal. Green below
  target, amber approaching the threshold, red above it.
- **Laser temperature** and **bed temperature** — °C
- **Supply height** and **bed height** — mm

Below them, the **cylinder visualisation** carried over from the existing app: a
drawn cross-section of the supply and build cylinders with their pistons at the
current height. It gives an instant sense of how much powder is left and how
deep the build has gone — keep it, it earns its space.

### Centre — the print

- **Layer progress** — 412 / 1668, a bar, current Z height in mm
- **Time** — elapsed, estimate remaining, estimated finish clock time
- **What it is doing right now** — Recoating · Marking · Waiting · Purging.
  During a recoat, which step; during purging, which stage.
- **The job** — name, when uploaded, layer count, thickness

### Right — chamber and safety

- **Interlock chain**: eleven indicators with names, not pin numbers —
  Main Chamber Door, Recirculation Door, Oxygen Sensor 1 and 2, six temperature
  channels, and System OK. Three states each: clear, warning, tripped.
- **Argon** — flowing or closed, litres used this job
- **Airflow** — chamber blower duty, radiator fan duty
- **Lighting** — off / ambient / shadow

### Alerts

A strip that is invisible when there is nothing wrong. Never a modal — a modal
over a running print hides the print.

---

## 7. Things the interface must never soften

Each of these exists because the honest version is not the obvious one.

### Substituted sensor readings

A sensor can be **overridden** in firmware — the machine reports a made-up value
so work can continue with a broken sensor. The oxygen sensor is overridden right
now.

The UI must show **both the substituted value and what the hardware actually
reads**, side by side, in red, wherever that channel appears. An overridden
oxygen reading looking like a normal oxygen reading is how someone opens a
chamber that is not safe.

There is also a whole-machine **test mode** where every sensor is faked. If that
is on, say so across the top of the application.

*(Sent on connect and on change only, never in the periodic report — so latch
it, do not wait for it.)*

### Position trust — three states, not two

Every axis is in one of three states, and they must look different:

| State | Show as | Means |
|---|---|---|
| **Unknown** | grey, no number | No reference. Software travel limits **cannot** be applied. Only the physical switches protect this axis. |
| **Homed** | green | Verified against a limit switch this power cycle. |
| **Restored** | amber or outlined | The position came back from the board's memory after a power cycle. Believed, not verified — nothing stops an axis being moved by hand while the machine is off. |

This matters because **the build and supply pistons have a limit switch at the
bottom only**. Driven far enough up they push the weight out of the cylinder.
The software limit is the only protection at that end and it needs a
trustworthy zero.

Normal life is: homed once, then restored on every boot after. **A startup
indicator showing each axis's trust state is a specific request** — it is the
first thing to check after any power interruption.

### Moving past the travel limits

There is a maintenance move that ignores the software limits: the pistons have
to be driven to the very top of their rail to get the build plate adapters out,
and there is no switch up there to stop them.

- Put it behind a **confirmation**, and behind a **user access level** when the
  application grows one.
- It applies to **one move only** — it is not a mode, and the UI must not make
  it look like a toggle that stays on.
- The board logs a warning every time it honours one. **Surface those in red.**
- The limit switches are never bypassed by anything. Say so, so nobody thinks
  this is more dangerous than it is.

### Which faults need clearing

Two kinds, and treating them alike makes the interface tiresome or dangerous:

- **Live** — an open door, an oxygen or temperature trip. Clears itself when the
  cause goes away. **Never ask for a fault-clear on these**, or the user clears a
  fault every time they load powder.
- **Latched** — an unexpected limit switch, a dead sensor. Needs an explicit
  clear, and if the cause is still there it comes straight back.
- **E-stop** — clears only on a physical board reset. Do not offer a button.

### Link errors are a cable problem

Both boards count CRC and framing errors. **A climbing count means a cable,
connector or electrical noise problem — not a software one.** It presents as
"commands sometimes don't work", which is exactly the symptom that gets
misdiagnosed for hours. Put the counters somewhere findable and say what a
rising number means.

---

## 8. Manual control

Setup and troubleshooting. Dense and precise; every control disabled with a
visible reason when the machine is in a state that will not accept it.

### Axes

Three: **Feed** (powder supply piston), **Bed** (build platform), **Wipe** (the
recoater blade).

Per axis, carried over from the existing app and worth keeping:

- Current position and target, mm to three decimals
- Trust state (§7)
- **Absolute / relative** toggle, a distance field, and direction arrows
- **Home** — and note homing takes a while: three approach passes, plus a travel
  measurement on the recoater

Plus **Home all**, and a global **Stop**.

Two things about the axes that shape the UI:

- **Motion is acknowledged when it finishes, not when it starts.** A thirty
  second move answers in thirty seconds. Show it as in-progress; do not treat
  the delay as a lost command. (The previous software's mysterious 120-second
  timeouts came from misreading exactly this.)
- **The recoater is the only open-loop axis.** It drifts and is re-homed
  periodically on its own. The pistons are closed loop.

### Recoat

Run one powder recoat cycle. Parameters: layer increment, settle time, park
mode, clearance, passes.

**Park mode deserves a real explanation in the UI**, because it is a genuine
trade-off the user is still measuring:

- **Overflow park** (default) — the blade finishes at the far end. The
  build-plate drop that makes room for the next layer also clears the return
  traverse, so powder is spread on the forward pass only and the blade never
  crosses fresh powder. Better surface.
- **Supply park** — the blade returns over the layer it just spread, so it needs
  an extra drop-and-raise of the most accuracy-critical axis in the machine.

### Chamber

- **Lighting**: off / ambient / shadow. Shadow is side-lighting — it is how
  surface topology, and therefore recoat defects, become visible to the camera.
  It is a diagnostic tool, not decoration.
- **Argon purge**: start / stop, target and progress (§9)
- **Chamber blower** and **radiator fan** duty. Note the radiator fan is not
  currently connected to anything.

### Laser

**Not available yet.** The firmware refuses every laser command deliberately.
Show the section with its controls disabled and a clear "not yet implemented"
rather than hiding it — the user knows it is coming and will look for it.

---

## 9. The purge

Its own view, because it runs for up to forty minutes and it is the thing
standing between setup and a print.

Three stages, and the order is physical rather than arbitrary. **Explain it in
the UI** — it is genuinely interesting and it stops people thinking it is stuck:

1. **Displace** (~8 min) — solenoid open, **blower off**. Argon is heavier than
   air and pushes it out by pressure. Stirring now would only mix them back
   together.
2. **Mix** — blower on, homogenising what is left, until oxygen falls below
   target. There is a **minimum time** here regardless of the reading, because
   oxygen reads low at the sensor long before the chamber is actually uniform,
   and the leftover pockets are what ruin a part.
3. **Verify** — solenoid **shut**, hold, and prove the chamber seals rather than
   merely reaching a number while gas is still flowing in.

**Show a plot of oxygen against its target over time.** That is the useful view;
a percentage-complete bar would be meaningless. Mark the stage transitions on it.
Show elapsed time and argon consumed.

A failed verification **stops nothing** — the machine reports it and leaves the
decision to the user. Make that a clear, prominent question rather than a
silent line in a log: *the chamber did not hold. Print anyway?*

---

## 10. Job

### Choosing and preparing

A job starts as a `.lpbf` file from the Lachesis slicer — an archive of one DXF
per layer plus a manifest. It has to be converted to a compact binary before it
goes to the machine.

Show, from the manifest, before anything is committed:

- Part names and count, layer count, layer thickness, total height
- The material profile in force, and the power and speed for each region type
  (contour, inskin, downskin, support)
- Estimated print time

**One thing to flag prominently:** the converter needs the laser's full-scale
power to express powers as a fraction. Newer exports carry it (500 W); older
ones do not. If it is missing, **ask** — do not quietly assume, because a wrong
full scale silently scales every power in the job.

Real numbers for sizing the UI: **1,500–1,700 layers, 2–5 million points,
19–43 MB** after conversion. Conversion takes two to five minutes.

### Uploading

Goes to the SD card on the galvo board, where it stays. Measured at **325 KB/s
— about a minute for 19 MB, two for 43 MB.**

- Progress in megabytes and an ETA, not a spinner
- **The final verification step takes several seconds** — the board reads the
  whole file back off the card to check it. Show it as a distinct "verifying"
  phase so it does not look like a hang. This step is what catches a card that
  accepted the bytes and stored something else.
- If it fails, the previous job on the card is **untouched**. Say so — it is
  reassuring and it is true.

### The card

Show what is on it: job id, layer count, size. It survives power cycles, so the
application should display what the board reports rather than only what it
uploaded itself.

### Running

Start, pause, resume, abort. Abort needs confirmation and should say plainly
that the build is not recoverable.

During a print: layer number and Z, current operation, time, and the running
argon total.

---

## 11. Sensors

Full detail, and history. The dashboard shows the two temperatures that matter;
this shows all eight channels.

- **Two oxygen channels** and **six temperature channels**
- Each has a **validity flag**. Four of the six temperature inputs have no
  sensor fitted — they read implausibly by design and are **permanently
  invalid**. That is correct, not a fault. Show them as "not fitted", distinct
  from a fitted channel that has failed.
- Overrides in red with the true value beside them (§7)
- **Plots over time**, with the interlock thresholds drawn as lines. Carried over
  from the existing app and heavily used.
- Sensor history should be browsable per job, alongside the layer images.

---

## 12. Camera

Carried over from the existing application, and central to how this machine is
actually debugged.

- Live view
- **Per-layer captures**: for each layer, images before and after marking, each
  in both **ambient** and **shadow** lighting. Four images per layer.
- A **layer browser** — scrub through the build, compare the same layer's four
  views, step forward and back. This is how a recoat defect is found after the
  fact, so it deserves real design attention rather than a thumbnail grid.
- Lighting needs a settle delay before capture (the camera has a physical lens
  and its autofocus and exposure need time). That delay is a setting, §13.

---

## 13. Settings

**New, and specifically requested.** These are values dialled in once per
machine and then rarely touched.

**They live on the boards, not in this application.** The app reads them back on
connect and writes changes. This matters: an application that keeps them itself
can only ever display what it last sent — which is wrong after any reset, wrong
on a second computer, and **wrong in the most dangerous way, because a settings
page showing the wrong numbers looks right**.

Out-of-range values are refused by the board rather than silently clamped, so
show the refusal against the field that caused it.

### Chamber and gas (Mega)

| Setting | Default | Note |
|---|---|---|
| Purge target oxygen | 3000 ppm (0.30 %) | depends on the alloy and how fussy the part is |
| Purge stage-2 timeout | 1800 s | depends on chamber volume and argon supply |
| Purge minimum mixing time | 60 s | a real floor, not padding — see §9 |
| Argon flow rate | 10 L/min | **a property of your regulator.** It is what the consumption figure is calculated from, so it wants calibrating. |

### Camera (Mega)

| Setting | Default |
|---|---|
| Light settle, ambient | 1500 ms |
| Light settle, shadow | 1000 ms |

Time to wait after switching lights before a capture is worth taking.

### Recoat (Mega)

| Setting | Default | Note |
|---|---|---|
| Settle time | 2000 ms | **The reason for this value is no longer known.** It costs about 33 minutes over a thousand layers, so it is worth understanding — but do not present it as free to reduce. Word the help text as "measure before changing". |
| Park mode | Overflow | §8 |
| Clearance | 0 | only meaningful with supply park |
| Passes | 1 | |

### Galvo (Teensy)

Not yet implemented in firmware. Leave room for it; do not design a page that
breaks when it is empty.

### Presenting settings well

Group by what they affect, not by which board holds them — the user does not
care which microcontroller stores a camera delay. Show each value's default and
whether it has been changed from it. Every setting wants a sentence of help
text; most of the sentences already exist above.

---

## 14. Calibration

### Field correction

The lens distorts the field; a 65×65 grid of offsets corrects it. Upload from a
`.cor` file.

Three refusals with genuinely different meanings — do not collapse them into
"upload failed":

| Refusal | Means |
|---|---|
| Bad scale | The field scale is outside 33–300 mm. Almost always the **1.0 placeholder** the old tooling wrote into every `.cor` and never filled in. |
| Out-of-order chunk | A chunk was lost. Restart the upload. |
| Bad checksum | Everything arrived but the table does not match its checksum. |

Show the table state: loaded or not, the scale in force, and the resulting field
size in mm.

**Worth telling the user plainly:** the `.cor` files that currently exist contain
**no distortion data at all** — they are pure linear ramps that only rescale the
field, and to a size that is not this machine's 175 mm. The lens has never
actually been characterised. Do not present an uploaded table as "calibrated".

A **"where does this point land"** probe — enter a bed coordinate, see the galvo
position — is the calibration question and the firmware already answers it.

### Measured values worth trending

The recoater measures its own track length against both switches on every home.
It was added to check whether a belt sprocket was giving the right distance, and
it is the first place a slipped belt or a shifted frame will show. **Plot it over
time**, do not just display the latest.

---

## 15. Log

Everything both boards have said, merged, timestamped, filterable by board and
severity. The existing application separates motor and laser logs; a single
stream with filters is better.

Log lines carry things with no structured field of their own — measured
recoater travel, argon consumed per purge, warnings when a move ran with limits
off. **Some of those are worth extracting into the interface** rather than
leaving buried; see §14 and §7.

---

## 16. States

| State | Meaning |
|---|---|
| **Boot** | powering up |
| **Idle** | alive, not ready |
| **Homing** | an axis is referencing itself |
| **Purging** | argon flowing (stage 1, 2 or 3) |
| **Ready** | homed and purged, will accept a job |
| **Printing** | |
| **Paused** | |
| **Fault** | recoverable, needs a clear |
| **E-stopped** | latched, needs a physical reset |

The two boards report their own states independently. The machine's state is a
combination, and the UI should be able to show either the summary or the
per-board detail.

---

## 17. What is not built yet

Design the interface as though these exist, but expect them to be greyed with
"not yet available":

- **Everything laser** — arming, power and speed parameters, marking, abort
- **Printing itself** — the sequencer that drives a job layer by layer
- **The print log** — the per-layer record of what actually happened
- **ESP32 airflow**, and the flow meter that would make closed-loop airflow real
- **Galvo settings**

And two that are refused for reasons worth surfacing if a user goes looking:

- **Soft reset of the Mega** — refused; a watchdog reset on that bootloader can
  brick it until it is manually reflashed
- **Speed-mapped airflow** — the idea is sound (faster scanning throws more
  spatter and wants more flow) but no board currently sees scan speed

---

## 18. Where the data comes from

Not needed to design the interface, but here so the wiring is unambiguous later.

- **`protocol/PROTOCOL.md`** — the complete wire format, every message and field
- **`docs/UI-REQUIREMENTS.md`** — the same ground as this document but from the
  firmware's side: exact message names, which values are published versus polled,
  which are sent only on change
- **`tools/`** — working Python that already does job conversion, job upload,
  correction upload and board probing. The conversion and upload logic can be
  lifted rather than rewritten.

Rates: sensors and safety once a second, heartbeats twice a second, axis
positions ten times a second while moving, purge progress every few seconds.
Overrides on change only.
