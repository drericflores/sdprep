# SDPrep – Update Notes and Operating Guide

**SD/microSD FAT32 Prep (GTK3) — SD CARD ONLY**
**Program by Dr. Eric Oliver Flores, enhanced Version**

## Purpose

SDPrep is a safety-focused Linux GUI tool to prepare **SD/microSD cards** as **FAT32**, intended for workflows like Raspberry Pi Pico / Pico W / Pico 2 projects where removable media must be reliably formatted without risking internal drives.

This version is explicitly engineered to:

* detect **SD/microSD only**
* avoid formatting system disks
* handle desktop automount behavior
* perform the destructive operations only with PolicyKit elevation

---

## What Was Updated (New in this Build)

### 1) SD Card Only Device Detection

**Change:** The device list is restricted to SD/microSD candidates only.
**Why:** Earlier builds could show USB SSD/HDD devices as “Maybe”, increasing risk.
**Result:** Large external drives are hidden and not selectable.

**Detection includes:**

* native SD devices like `/dev/mmcblk*`
* USB-attached SD readers that present removable media as `/dev/sdX` with `rm=1`

---

### 2) Fixed `lsblk` Query Error (Drop-down Works Again)

**Change:** Removed incompatible `lsblk` options that caused:

* `mutually exclusive arguments: --output-all --output`

**Why:** Some `lsblk` versions reject combining `-O` and `-o`.
**Result:** The device dropdown populates correctly.

---

### 3) `pkexec` Launch Reliability Fix (No PATH Assumptions)

**Change:** The formatter no longer tries to run `pkexec` by name only. It uses an **absolute path** lookup:

* `/usr/bin/pkexec` (preferred)
* `/bin/pkexec` (fallback)

**Why:** GUI processes often don’t inherit a normal shell PATH (especially under Wayland).
**Result:** Eliminates:
`Failed to execute child process "pkexec" (No such file or directory)`

---

### 4) Automatic Unmounting (Pre-Step + Privileged Recheck)

**Change:** SDPrep now attempts to unmount any mounted partitions on the selected SD card:

* **before** formatting (best effort in GUI context)
* **again inside pkexec** immediately before wiping/partitioning

**Why:** Desktop automounters can remount a device between unmount and formatting, causing safety checks to fail or formatting to hang.
**Result:** Formatting is more consistent and safer.

Unmount methods used:

* `udisksctl unmount -b <partition>`
* fallback to `umount <partition>`

---

### 5) Safety Check Hardened and Debuggable

**Change:** The privileged formatter prints hardware flags it reads from `lsblk`:

* `TYPE`
* `RO`

**Why:** Some devices report unexpected metadata. This makes failures explainable rather than mysterious.

**Important fix included:**
The RO (read-only) value sometimes arrives as `" 0"` with leading whitespace.
We now **trim whitespace** before comparing.

**Result:** Prevents false error:
`ERROR: device is read-only (RO= 0).`

---

### 6) Confirmation Workflow Simplified

**Change:** Removed the “type ERASE” confirmation requirement.
**Now:** one clear warning dialog + Format button.
**Why:** You requested click-to-format with minimal friction (still safe via a warning prompt).

---

### 7) Improved Logging (See Exactly What Happened)

**Change:** SDPrep streams stdout/stderr from the privileged formatter into the GUI log window.
**Why:** When something fails, you can see whether it happened at wipefs, partition, mkfs, etc.

---

## Current Formatting Behavior (What It Does)

When you click **Format FAT32**, SDPrep performs:

1. Detect selected SD card device (example: `/dev/sdf`)
2. Attempt to unmount any mounted partitions
3. Run a privileged formatter via `pkexec`:

   * verifies device type and read-only flag
   * ensures no partitions are mounted
   * wipes signatures: `wipefs -a`
   * creates MBR table: `parted mklabel msdos`
   * creates two partitions:

     * **Partition 1:** FAT32 from 1MiB to (end − 32MiB)
     * **Partition 2:** remainder (reserved / future use)
   * formats Partition 1 FAT32:

     * `mkfs.fat -F32 -n <LABEL>`

This layout keeps a small reserved area (useful for certain embedded workflows), while still producing a standard FAT32 volume usable on Linux/Windows.

---

## How to Build

From the folder containing `sdprep.c`:

```bash
gcc -O2 -Wall -Wextra -std=c11 sdprep.c -o sdprep `pkg-config --cflags --libs gtk+-3.0 json-c`
```

Required packages (Ubuntu/Pop!_OS):

```bash
sudo apt-get install -y build-essential pkg-config libgtk-3-dev libjson-c-dev policykit-1
```

---

## How to Run (Recommended)

Run normally as your user:

```bash
./sdprep
```

When you press **Format**, the system will prompt for admin authorization via PolicyKit.

**Do not** run the GUI with `sudo` (that commonly breaks display permissions under Wayland).

---

## Expected Device Example

A healthy detection line looks like:

* `/dev/sdf  MassStorageClass  [58G] (tran=usb rm=1)`

This commonly appears for microSD cards in USB readers.

---

## If Formatting Fails

Use the Log window:

* If it stops at `[1/7] Safety check...`
  the log will show `TYPE` and `RO` and a specific reason.

* If it says “still mounted”
  close any open file browser windows and retry. Some desktops aggressively remount cards.

---

## Safety Guarantees (What SDPrep Refuses to Do)

* It does **not** intentionally list internal HDD/SSD devices in this SD-only build.
* It refuses read-only targets.
* It refuses to format while partitions remain mounted.
* It performs destructive operations only inside `pkexec` (privileged boundary).

---

## Version Credit

**SDPrep**
Program by **Dr. Eric Oliver Flores**
Version 2 enhanced 

