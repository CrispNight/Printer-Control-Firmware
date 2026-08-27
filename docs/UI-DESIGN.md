# Printer control software — design brief

The desktop application that drives the printer. This brief exists to give a
designer everything they need without having to ask: what the machine is, what
the software has to do, and which of the two is negotiable.

It is deliberately split into three parts, because they carry different weight:

| Part | Weight |
|---|---|
| **1 — The machine** | Facts. Not opinions, not preferences. They come from hardware and physics and cannot be designed around. |
| **2 — What the software does** | Scope. The features that need to exist and the behaviour that would be wrong to get backwards. |
| **3 — Brand and craft** | Direction. The palette and type are fixed by the brand; the rest is the designer's call. |

Part 1 is short on purpose. Everything in it earns its place — most items are
there because getting them wrong has a real consequence, and those consequences
are named so nobody has to take it on trust.

*Moiren is the company that builds the machine, not the name of this software.
The application can be called whatever suits it.*

---

# Part 1 — The machine

## 1.1 What it is

A **selective laser melting (SLM) metal 3D printer**. It spreads a 30 µm layer
of metal powder and melts a cross-section into it with a **500 W fibre laser**,
then repeats — **1,500 to 1,700 times** for a typical part. A print runs for
many hours, usually overnight.

The chamber is flooded with **argon**. Powder plus oxygen plus a laser is a
fire; the purge is what prevents that, and it takes **up to forty minutes**
before a print can begin.

Three boards run it:

| Board | Owns |
|---|---|
| **Teensy 4.1** (galvo card) | the laser, the mirrors that steer it, the job file on its SD card |
| **Arduino Mega** | three axes, oxygen and temperature sensing, the interlock chain, argon and airflow, chamber lighting |
| **ESP32** | airflow, eventually. Not built — ignore it. |

## 1.2 The software is optional

The printer completes a job with the computer switched off or unplugged. This
application adds a user interface, job upload and camera monitoring. Nothing a
print depends on runs here.

Two consequences worth designing around: **losing the connection is not an
emergency**, and reconnecting mid-print should pick up an accurate picture from
the boards rather than from anything the app remembered.

## 1.3 Emergency stop is physical

**There is no software E-stop and there should not be one.** It is a hardware
switch on the machine.

The software still needs to *show* an e-stopped state — the boards latch it and
report it, and it clears only on a physical reset — but it never causes one.

Stopping a *print* is a different thing and does belong here: see §2.3.

## 1.4 The pistons have no upper limit switch

The build and supply cylinders have a switch at the **bottom only**. Driven far
enough up they push the weight out of the cylinder.

The software travel limit is the only protection at that end, and a software
limit needs a trustworthy zero — which is why axis position has **three** states
rather than two:

| State | Means |
|---|---|
| **Unknown** | No reference. Travel limits cannot be applied at all; only the physical switches protect the axis. |
| **Homed** | Verified against a limit switch this power cycle. |
| **Restored** | Position came back from the board's memory after a power cycle. Believed, but not verified — nothing stops an axis being moved by hand while the machine is off. |

Normal life is homed once, then restored on every boot after. The three need to
be distinguishable at a glance, and **a per-axis indicator at startup was
specifically asked for** — it is the first thing to check after any power
interruption.

There is also a maintenance move that ignores the software limits, because the
pistons have to be driven to the very top of the rail to remove the build plate
adapters. It is per-move rather than a mode, and the firmware logs a warning
each time it honours one. The limit switches themselves are never bypassed by
anything.

## 1.5 A sensor reading may not be a sensor reading

Firmware can **override** a channel — report a made-up value so work can
continue with a broken sensor. **The oxygen override is on right now.** There is
also a whole-machine test mode where every channel is faked.

The board reports the substituted value *and* what the hardware actually reads,
precisely so both can be shown. An overridden oxygen reading that looks like an
ordinary oxygen reading is how someone opens a chamber that is not safe.

*(Sent on connect and on change only, never in the periodic report — so it has
to be latched rather than waited for.)*

## 1.6 Four of the six temperature inputs have no sensor

They read implausibly by design and are **permanently invalid**. That is
correct, not a fault. They are distinguishable from a fitted channel that has
failed, and want to look different.

## 1.7 Two kinds of fault

- **Live** — an open door, an oxygen or temperature trip. Clears itself when
  the cause goes away.
- **Latched** — an unexpected limit switch, a dead sensor. Needs an explicit
  clear, and comes straight back if the cause is still present.

Asking for a clear on a live fault means clearing one every time powder is
loaded. E-stop is neither: physical reset only.

## 1.8 Link errors mean a cable

Both boards count CRC and framing errors. A climbing count is a cable,
connector or noise problem — **not software**. It presents as "commands
sometimes don't work", which is the symptom that gets misdiagnosed for hours.

## 1.9 Motion answers when it finishes

A move, a home, or a recoat is acknowledged on **completion**, not acceptance. A
thirty-second move answers in thirty seconds. The previous software's
mysterious 120-second timeouts came from misreading exactly this.

## 1.10 Timings and sizes, measured

| | |
|---|---|
| Purge | up to 40 min — 8 min displace, then mix, then verify |
| Job, converted | 1,500–1,700 layers · 2–5 M points · **19–43 MB** |
| Conversion | 2–5 min |
| Upload to the card | **325 KB/s** — about a minute for 19 MB |
| Upload verify step | seconds, not milliseconds — the board reads the file back |
| Print | many hours |

---

# Part 2 — What the software does

## 2.1 Who it is for

One person, in a workshop, who built this machine. At a desk beside it, or on a
laptop checking an overnight run. Not an operator following a procedure — the
engineer, reading the interface to work out why something went wrong.

Two moods: **watching** (a print is running, hours to go — is it fine?) and
**working** (setting up, troubleshooting, maintenance).

## 2.2 Feature areas

Roughly eight, however they end up arranged:

| Area | What it covers |
|---|---|
| **Overview** | The screen that is up for fifteen hours. Vital signs, print progress, camera, chamber and interlock state. |
| **Manual control** | Axis jogging and homing, recoat cycles, lighting, argon and airflow. |
| **Job** | Choose a file, convert it, upload it, run it. |
| **Sensors** | All eight channels in detail, with history. |
| **Camera** | Live feed, and the per-layer image record. |
| **Settings** | Machine values dialled in once. |
| **Calibration** | Field correction, and measurements worth trending. |
| **Log** | Everything both boards have said. |

Some machine state — state, connection, alerts, print progress — wants to be
visible from anywhere rather than living on one screen.

## 2.3 Print control

Start, pause, resume, and stop a job. Pause and resume are ordinary; **stop is
destructive** and the build will not be recoverable, so it warrants confirming.

Not an emergency stop — that is the physical switch (§1.3).

## 2.4 The camera

Both halves matter:

- **Live feed** from a USB camera, whenever one is connected. This is how the
  machine is watched during a print, so it deserves real space rather than a
  thumbnail — very likely on the main overview as well as its own screen.
- **The per-layer record**: for each layer, images before and after marking, in
  both **ambient** and **shadow** lighting — four per layer. Shadow is
  side-lighting, and it is how surface topology and therefore recoat defects
  become visible. Being able to scrub through the build and compare a layer's
  four views is how a defect gets found after the fact.

Switching lights needs a settle delay before a capture is worth taking (the
camera has a physical lens; focus and exposure need a moment). That delay is a
setting.

## 2.5 Axes and recoat

Three axes: **Feed** (powder supply piston), **Bed** (build platform), **Wipe**
(the recoater blade). Each needs position, target, trust state (§1.4), jogging
in absolute or relative terms, and homing.

The recoater is the only **open-loop** axis — it drifts, and re-homes itself
periodically. The pistons are closed loop.

A **recoat cycle** is one powder pass, with a layer increment, settle time, park
mode, clearance and pass count. Park mode is worth explaining wherever it is
offered, because it is a real trade-off still being measured:

- **Overflow park** (default) — the blade finishes at the far end. The
  build-plate drop that makes room for the next layer also clears the return
  traverse, so powder is spread on the forward pass only and the blade never
  crosses fresh powder. Better surface.
- **Supply park** — the blade returns over the layer it just spread, needing an
  extra drop-and-raise of the most accuracy-critical axis in the machine.

## 2.6 The purge

Three stages, in a physical order worth surfacing so it does not look stuck:

1. **Displace** (~8 min) — solenoid open, **blower off**. Argon is heavier than
   air and pushes it out by pressure; stirring now would only remix them.
2. **Mix** — blower on, homogenising what is left, until oxygen falls below
   target. There is a minimum time here regardless of the reading, because
   oxygen reads low at the sensor long before the chamber is uniform.
3. **Verify** — solenoid **shut**, hold, and prove the chamber seals rather than
   merely reaching a number while gas is still flowing in.

Oxygen against its target over time is the meaningful view of progress; a
percentage would not mean much. Elapsed time and argon consumed are both
reported.

A failed verification **stops nothing** — the machine reports it and leaves the
decision to the user. That is a real question to put to them, not a log line.

## 2.7 Job preparation and upload

A job arrives as a `.lpbf` from the Lachesis slicer — an archive of one DXF per
layer plus a manifest — and is converted to a compact binary before it goes to
the machine. Worth showing from the manifest before anything is committed: parts,
layer count and thickness, total height, the material profile, and the power and
speed for each region type (contour, inskin, downskin, support).

The converter needs the laser's full-scale power to express powers as a
fraction. Newer exports carry it (500 W); older ones do not. **When it is
missing the software should ask rather than assume** — a wrong full scale
silently scales every power in the job.

Upload goes to the SD card on the galvo board and stays there, surviving power
cycles. Progress in megabytes rather than a spinner, and the final **verify**
phase is worth showing as its own step so it does not look like a hang. A failed
upload leaves the previous job untouched.

## 2.8 Settings live on the boards

The application reads them back on connect and writes changes; it does not keep
its own copy. An app that remembers them can only display what it last sent —
wrong after any reset, wrong on a second computer, and wrong in the worst way,
because a settings page showing the wrong numbers looks right.

Out-of-range values are refused rather than clamped, so a refusal wants showing
against the field that caused it.

| Setting | Default | Note |
|---|---|---|
| Purge target oxygen | 3000 ppm (0.30 %) | depends on the alloy and how fussy the part is |
| Purge stage-2 timeout | 1800 s | depends on chamber volume and argon supply |
| Purge minimum mixing | 60 s | a real floor, not padding — see §2.6 |
| Argon flow rate | 10 L/min | a property of the regulator; the consumption figure is derived from it |
| Light settle, ambient | 1500 ms | how long after switching before a capture is worth taking |
| Light settle, shadow | 1000 ms | |
| Recoat settle | 2000 ms | the reason for this value is no longer known. It costs ~33 min over a thousand layers, so it is worth understanding — but it is not free to reduce |
| Recoat park mode | Overflow | §2.5 |
| Recoat clearance | 0 | only meaningful with supply park |
| Recoat passes | 1 | |

Grouping by what they affect will read better than grouping by which board
holds them — nobody cares which microcontroller stores a camera delay.

## 2.9 Calibration

A 65×65 grid of offsets corrects lens distortion, uploaded from a `.cor` file.
Three refusals with genuinely different meanings, worth telling apart rather
than collapsing into "upload failed":

| Refusal | Means |
|---|---|
| Bad scale | Field scale outside 33–300 mm. Almost always the **1.0 placeholder** the old tooling wrote into every `.cor` and never filled in. |
| Out-of-order chunk | A chunk was lost. Restart the upload. |
| Bad checksum | Everything arrived, but the table does not match its checksum. |

Worth knowing: **the `.cor` files that exist contain no distortion data at all.**
They are pure linear ramps that only rescale the field, and to a size that is
not this machine's 175 mm. The lens has never been characterised, so an uploaded
table should not be presented as "calibrated".

The firmware can answer "where does this bed coordinate land on the galvo",
which is the useful calibration probe.

The recoater also measures its own track length on every home. It was added to
check whether a belt sprocket was giving the right distance, and it is the first
place a slipped belt or shifted frame shows — so it is worth trending rather
than displaying once.

## 2.10 Not built yet

Design as though these exist, but expect them unavailable — the firmware
refuses them deliberately rather than pretending, so showing them as
unavailable is accurate rather than an error:

Laser arming, laser parameters, marking, mark abort · the print sequencer ·
the per-layer print log · ESP32 airflow and the flow meter · galvo settings.

Two more are refused for reasons worth surfacing if someone goes looking: a
**soft reset of the Mega** (a watchdog reset on that bootloader can brick it
until manually reflashed) and **speed-mapped airflow** (sound idea — faster
scanning throws more spatter and wants more flow — but no board currently sees
scan speed).

---

# Part 3 — Brand and craft

## 3.1 Palette — fixed

Taken from the Moiren site. These are the brand, not suggestions.

| Role | Hex |
|---|---|
| Ink, and the wordmark | `#1d2b31` |
| Ground | `#f4f4f2` |
| Surface, warm white | `#faf9f5` |
| Accent | `#35798a` |
| Accent, deeper (hover, headings) | `#1d444f` |
| Accent, lighter | `#5fa3b2` |
| Mid slate | `#4a5b61` |
| Muted slate | `#6f8187` |
| Rule / border | `#dcdcd7` |
| Warm grey | `#c9cdc7` |
| Rust | `#a04434` |

Slate and teal, warm neutrals. The site sets teal on white for its primary
action and ink on ground for its secondary.

**A dark theme will have to be derived** — the site is light only, and this
application gets watched in a workshop at night. Keep the same character: slate
grounds rather than pure black, and the teal adjusted to hold up on a dark
surface rather than swapped for something else.

## 3.2 Type — fixed

Also from the site:

| Role | Face |
|---|---|
| Display, headings, buttons | **Space Grotesk** (500, 600) |
| Body | **IBM Plex Sans** |
| Numbers, labels, logs | **IBM Plex Mono** |

The site uses IBM Plex Mono for small letter-spaced labels and section numerals,
and Space Grotesk with wide letter-spacing for the wordmark. Both habits carry
over well.

Anything that changes while being read — a temperature, a position, a layer
count — wants tabular figures so the digits do not jump.

## 3.3 Semantic colour

State colour is separate from the brand accent, and the accent should not be
spent on it. Roughly: something for good, something for a warning band,
something for tripped or overridden, something for unknown. `#a04434` is the
brand's rust and is the obvious starting point for the alarming end.

**Unknown must not look like fine.** A sensor that is not reporting is not a
sensor reading zero; an axis with no reference is not an axis at zero. That
distinction needs to survive in whatever visual language gets chosen.

## 3.4 What actually matters

Not layout instructions — the things that make this interface good or bad:

- **Readable, first.** Someone is reading this at 2am to work out what went
  wrong. Legibility of numbers beats density of them.
- **Glanceable when watching, dense when working.** Those two moods want
  different things; how to serve both is an open question.
- **Long operations show what is changing.** A purge is forty minutes, an
  upload two, a print fifteen hours. Oxygen falling toward a target, megabytes
  transferred, layers completed — something real, rather than an indeterminate
  animation.
- **Alerts should not hide the print.** A running job is the thing being
  watched.
- **Clean.** The brand is calm and spare, and the machine is an instrument. The
  interface should feel like the same object.

## 3.5 Room to move

Everything not in Part 1 is open, and a few things are worth saying explicitly
because an earlier draft of this brief over-specified them:

Screen count and arrangement · navigation pattern · whether the overview is a
grid, columns, or something else · which theme is the default · window size and
responsive behaviour · whether the existing prototype's cylinder cross-section
drawing survives (it is genuinely useful — it shows powder remaining and build
depth at a glance — but it is not sacred) · chart styling · how alerts are
surfaced · typographic scale and spacing.

**The existing prototype is a reference, not a target.** It is a Qt application
with a left sidebar of stat cards and a dense main panel. It works and its user
is used to it, which is worth something — but it was never designed, and this is
the chance to do that.

---

## Where the data comes from

Not needed to design the interface. Here so the wiring is unambiguous later.

- `protocol/PROTOCOL.md` — the complete wire format, every message and field
- `docs/UI-REQUIREMENTS.md` — the same ground from the firmware's side: exact
  message names, which values are published versus polled, which are sent only
  on change
- `tools/` — working Python that already does job conversion, job upload,
  correction upload and board probing, liftable rather than rewritable

Update rates: sensors and safety once a second, heartbeats twice a second, axis
positions ten times a second while moving, purge progress every few seconds,
overrides on change only.
