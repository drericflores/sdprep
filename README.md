
````markdown
# SDPrep — PicoCalc SD/USB Formatter

**Author:** Dr. Eric Oliver Flores Toro  
**Contact:** [eoftoro@gmail.com](mailto:eoftoro@gmail.com)  
**Version:** 1.0  
**License:** MIT  

---

##Overview

** SDPrep** is a professional, GUI-driven storage preparation utility designed to **format and partition SD cards and USB storage** for use with the **Clockwork Pi PicoCalc** and other embedded systems.

Unlike generic formatters, SDPrep:

- Enforces **device-type awareness** — only removable USB or MMC media are listed.  
- Prevents **system-disk damage** by blocking internal drives.  
- Creates a **precisely defined two-partition layout** compatible with firmware update and dual-image workflows.  
- Uses a modern **GTK 4 interface** with real-time logging and progress feedback.  
- Performs all operations safely with a single click.

---

## Build & Integration

### Dependencies

Install the required development packages on Pop!\_OS / Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  libgtk-4-dev \
  libjson-glib-dev \
  build-essential \
  pkg-config \
  parted \
  dosfstools \
  util-linux \
  udev
````

---

### Compile

Build the executable:

```bash
gcc -O2 -Wall -Wextra -o sdprep sdprep.c \
    $(pkg-config --cflags --libs gtk4 json-glib-1.0)
```

Run with elevated privileges:

```bash
sudo ./sdprep
```

---

### Optional Install

Install system-wide:

```bash
sudo install -m755 sdprep /usr/local/bin/
```

You can also add a launcher by creating a file named
`sdprep.desktop` under:

```
~/.local/share/applications/
```

Example desktop entry:

```ini
[Desktop Entry]
Type=Application
Name=SDPrep
Comment=PicoCalc SD/USB Formatter
Exec=sudo /usr/local/bin/sdprep
Icon=media-removable
Terminal=false
Categories=Utility;System;
```

---

## Features

* **Safe device detection:** Only removable USB/MMC devices appear.
* **Auto-partitioning:** Creates a large FAT32 partition and a 32 MB reserved tail.
* **Visual feedback:** Progress bar, live log, and abort controls.
* **Confirmation required:** Prevents accidental formatting of system drives.
* **Cross-platform layout:** Identical to the official `partition_usb_32mb.sh` script.

---

## Example Output

```text
/dev/sdb1  → 57.9 G  FAT32  Label=MICROPYTHON
/dev/sdb2  → 32 M   (reserved tail)
```

---

## Purpose

SDPrep provides engineers and everyday users with a **safe, deterministic, and auditable** method to create storage media for embedded systems like PicoCalc.
It embodies *metrology-grade precision* and *human-centered safety design* in a single, open-source tool.

---

## GitHub Release Download

After publishing your release, users can download SDPrep directly:

```bash
wget https://github.com/ericoliverflores/SDPrep/releases/download/v1.0/sdprep
chmod +x sdprep
sudo ./sdprep
```

*(Replace `ericoliverflores` with your GitHub username if different.)*

---

**© 2025 Dr. Eric Oliver Flores Toro**

```

