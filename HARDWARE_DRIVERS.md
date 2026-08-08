# Kernel drivers (`/module_driver`)

Reference for the vendor out-of-tree kernel modules loaded at boot on the real
device, extracted by reading `/module_driver/*.sh` (the `insmod` invocations
with their parameters) and running `strings` against the matching `.ko`
binaries in `squashfs-root/module_driver/` (no kernel source was available --
everything below is derived from the loader scripts, the module's own
embedded `MODULE_DESCRIPTION`/parameter strings, exported symbols, and printk
format strings, cross-referenced with live `sysfs`/`dmesg` behavior observed
on a real unit). Kernel: `4.4.94+`, MIPS32 R2, Ingenic X1600 SoC.

Load order (from `driver_default_init_script.sh`, which also writes
per-module boot timing to `/tmp/boot_profile.log`):

```
utils -> rmem_manager -> soc_utils -> soc_i2c -> i2c_gpio_add -> axp2101 ->
cw2015 -> keyboard_gpio_add -> soc_gpio -> soc_pwm -> pwm_backlight ->
soc_fb -> soc_aic -> soc_msc -> soc_adc -> lcd_lg35583 -> sa_sound_switch ->
codec_cs43131 -> cst8xx_touch -> cywdhd -> keyboard_adc_multifunc ->
leds_pwm_add -> sa_config -> sa_earpods_adc -> sa_hgl_dma -> sau ->
soc_efuse -> tcs1421 -> x1600_hiby_r1_sound_card
```

This entire sequence runs before `hiby_player` (stock or this project's
build) ever starts, and is identical regardless of which player app is
flashed -- nothing in this app's own source touches `/module_driver` or its
load order.

## Full driver list

| Module | Description (from binary) | Role |
|---|---|---|
| `utils.ko` | (none) | Base utility symbols other vendor modules link against |
| `rmem_manager.ko` | Ingenic rmem_manager driver | Reserved-memory region manager, sized from the `rmem=` kernel cmdline arg |
| `soc_utils.ko` | (none) | SoC-wide utility symbols (`rtc32k_init_on=0`) |
| `soc_i2c.ko` | X1600 SoC I2C driver | Two hardware I2C buses (bus 0 @ 400kHz on PA28/PA29, bus 1 @ 400kHz on PB19/PB20) |
| `i2c_gpio_add.ko` | Platform-independent bitbanging I2C driver | A **third**, bit-banged I2C bus (bus 3, 200kHz on PB30/PB31) -- this is the bus the audio codec (`codec_cs43131`, `cs43131_i2c_bus_num=3`) actually lives on, not one of the two hardware I2C controllers |
| `axp2101.ko` | PMIC MFD core/I2C/regulator driver for AXP20X | Main power-management IC -- defines every voltage rail (`dcdc1-4`, `aldo1-4`, `bldo1-2`, `dldo1-2`) other drivers reference by name (e.g. `cst8xx_touch`'s `cst_regulator_name=aldo2`, `lcd_lg35583`'s `bldo1`/`bldo2`) |
| `cw2015.ko` | CW2015 FGADC Device Driver | Battery fuel gauge (battery.c's real hardware backend) |
| `keyboard_gpio_add.ko` | Keyboard driver for GPIOs | Physical power (`PC31`) and next-track (`PC28`) buttons -- registers as real Linux input `KEY_POWER`/`KEY_NEXTSONG` events, matching hw_buttons.c's own evdev reader |
| `soc_gpio.ko` | JZ x1600 gpio driver | Core SoC GPIO controller (4 banks of 32 -- `PA00-31`=0-31, `PB00-31`=32-63, `PC00-31`=64-95, `PD00-31`=96-127; e.g. `PB02` = global GPIO 34) |
| `soc_pwm.ko` | Ingenic SoC PWM driver | PWM channel infrastructure (backlight, LEDs both ride on this) |
| `pwm_backlight.ko` | pwm_backlight driver | LCD backlight (`PC00`, 0-101 brightness range -- matches backlight.c's own 0-100 scale plus a top step) |
| `soc_fb.ko` | Ingenic Soc FB driver | Framebuffer (`/dev/fb0`), double-buffered (`frame_num=2`) |
| `soc_aic.ko` | (none) | **Audio Interface Controller** -- the SoC-side I2S/DMA engine feeding the codec. See "Audio subsystem" below. |
| `soc_msc.ko` | Multimedia Card Interface driver | Both storage controllers: `msc0` = internal eMMC/flash (non-removable), `msc1` = the physical SD card slot (`PC25` power, `PB22` card-detect) -- this is `/dev/mmcblk0`, mounted at `/usr/data/mnt/sd_0` |
| `soc_adc.ko` | JZ x1600 ADC driver | Shared ADC block (`adc_vref=3300`mV) -- multiple other drivers read specific channels off this (headset detection, headset remote buttons, the physical volume/play-pause/prev "ADC keyboard") |
| `lcd_lg35583.ko` | lg35583 lcd panel driver | The actual LCD panel (SPI bus 5, `bldo1`/`bldo2` power rails) |
| `sa_sound_switch.ko` | SmartAction Sound Switch | Headset **presence** detection only (not a physical audio relay despite the name). See "Audio subsystem" below. |
| `codec_cs43131.ko` | Soc CS43131 driver | The actual DAC/headphone-amp chip. See "Audio subsystem" below. |
| `cst8xx_touch.ko` | hyn Touchscreen Driver | Capacitive touch panel (I2C bus 1 @ `0x15`, 480x800, `PA17` reset/`PA16` irq, powered off `aldo2`) |
| `cywdhd.ko` | Bluetooth power control driver | Wi-Fi/BT combo chip power sequencing (`PB03` wlan reg-on, `PB04` bt reg-on, `PB05` host-wake-bt) -- **not** the Bluetooth stack itself (that's bluetoothd/BlueZ in userspace), just the chip's own power-up GPIOs. Its own `MODULE_DESCRIPTION` undersells it: `strings` shows this is actually the **full Broadcom/Cypress `dhd` WiFi SDIO driver** (`dhd_bus_*`, `bcmsdh_*`, CYW43364/43438/43455/4354 firmware paths) -- the BT power GPIOs are a side feature riding on the same combo-chip driver, not a dedicated BT module. See "Investigating the AVRCP playback-control bug" below for why this module turned out to be a dead end for that specific issue. |
| `keyboard_adc_multifunc.ko` | x2000 ADC_KEYBOARD driver | The physical volume/play-pause/prev buttons, read via a resistor-ladder on ADC channel 0 -- `key1`=`KEY_PLAYPAUSE`(164)/reuse 165, `key2`=`KEY_VOLUMEUP`(114)/reuse `KEY_VOLUMEDOWN`(115) at ADC 50-250, `key3`= the same pair inverted at ADC 350-550 (two more physical buttons sharing the "reuse" mechanism for a long-press-alternate-function pattern) |
| `leds_pwm_add.ko` | PWM Device for LED | Two PWM LEDs: `PC01`=red (`breathing` trigger, i.e. hardware-driven pulse, matching LED_control.c's own findings about broken/limited kernel triggers), `PC02`=blue (`none` trigger, plain on/off) |
| `sa_config_module.ko` | SmartAction configs driver | No parameters -- likely just exposes some vendor config sysfs tree, not otherwise identified |
| `sa_earpods_adc.ko` | Earpods adc For APPLE | Headset **remote button** detection (play/pause/volume via an in-line mic/remote's resistor ladder). See "Audio subsystem" below. |
| `sa_hgl_dma.ko` | Smartaction user dma driver | A 6MB (`sahd_hgl_mem_size=6291456`) DMA buffer pool, purpose not otherwise identified from strings alone |
| `sau.ko` | SmartAction Information | No parameters, minimal module -- not otherwise identified |
| `soc_efuse.ko` | JZ x1600 EFUSE driver | SoC one-time-programmable fuse readout (serials/calibration data) |
| `tcs1421_add.ko` | TCS1421 for USB | USB Type-C/PD controller (`PA09`/`PB24` config pins) -- almost certainly what usb_mode_control.c's mode switching ultimately drives |
| `x1600_hiby_r1_sound_card.ko` | (none) | The **ASoC machine driver** tying the AIC, codec, and DAPM routes together into one registered sound card. See "Audio subsystem" below. |

## Audio subsystem (the modules relevant to the headphone-jack pop investigation)

Five modules cooperate to form the whole audio path:

```
soc_aic (I2S/DMA engine)
        |
x1600_hiby_r1_sound_card (ASoC machine driver -- DAPM routes, calls
        |                  cs43131_set_power() to gate codec power)
        v
codec_cs43131 (the DAC/amp chip itself, I2C bus 3 @ addr 0x30)

sa_sound_switch (headset PRESENCE detection, ADC channel 1, 200ms debounce)
        |
        v  (get_switch_status(), an exported kernel symbol)
sa_earpods_adc (headset REMOTE BUTTON detection, ADC channel 2 -- only
                meaningful once sa_sound_switch confirms a headset is
                actually present; registers a real Linux input device,
                "earpods-adc/input0", for play/pause/volume presses)
```

### `soc_aic.ko` -- I2S/DMA engine

No `MODULE_DESCRIPTION`, but its parameters and strings reveal a **built-in
anti-pop mechanism at the SoC level**, independent of anything either
userspace player does:

- `aic_if_send_invalid_data=1`, `aic_if_send_invalid_data_everytime=0`,
  `aic_send_invalid_data_time_ms=120` -- when a real PCM stream stops, the
  I2S bit/word clock keeps running with "invalid" (silent/dummy) data for
  120ms afterward, rather than the clock stopping instantly. This is a
  classic I2S anti-pop technique: an abruptly-stopped BCLK/LRCLK forces the
  codec's internal PLL to re-lock on the next start, and many codecs produce
  an audible artifact during that relock -- keeping the clock alive briefly
  avoids the relock entirely for a quick stop/restart.

### `x1600_hiby_r1_sound_card.ko` -- ASoC machine driver

Minimal module (no other identifying strings), but its one relevant symbol
reference is decisive: it calls `cs43131_set_power()`, the codec driver's own
**exported kernel-symbol** power-gate function (see below). This confirms
codec chip power is gated automatically by the standard ALSA/DAPM bias-level
state machine as the machine driver reacts to stream activity -- not
something either userspace player can (or needs to) call directly. Standard
ASoC behavior here also implies a `pmdown_time` (default ~5000ms in
mainline ALSA) between a stream actually stopping and DAPM triggering the
real power-down/mute sequence, specifically to avoid pop-prone rapid power
cycling on quick pause/resume.

### `codec_cs43131.ko` -- the DAC/amp chip

Boot config (`codec_cs43131.sh`): I2C bus 3, power GPIO `PB02` (= global GPIO
34, active-high), reset GPIO `PB21` (active-high), **no** mute GPIO
(`cs43131_mute_gpio=-1`) and **no** "po_sel" GPIO (`cs43131_po_sel_gpio=-1`)
-- both left permanently unconfigured on this board, meaning any code path in
the driver that only fires when those GPIOs are actually assigned is
dead/unreachable here.

Confirmed real-device findings (this investigation, live testing against
`/sys/bus/i2c/devices/3-0030/`):

- **`cs43131_dai_digital_mute`** -- the driver implements ASoC's standard
  `.digital_mute` DAI callback, automatically invoked by the framework around
  stream start/stop. This is very likely *why* this app's existing
  pause-time pop fix (closing the PCM device the instant pause is detected --
  see `audio.c`) actually works: it isn't manually muting anything itself,
  it's triggering the kernel's own already-correct stop sequence (digital
  mute -> the AIC's 120ms invalid-data tail -> eventually `cs43131_set_power`
  cutting real power), rather than leaving the stream "prepared" and the
  codec fully live indefinitely while paused.
- **`cs43131_mute_put`** (the ALSA "Mute Output" simple-mixer-control write
  handler) and the exposed `"Mute Output"`/`"Soft Mute"` controls **do not
  actually hold state when set from userspace** -- confirmed directly:
  `amixer cset numid=3 on` reports success in its own echo, but an immediate
  follow-up read shows `off` again, under every condition tested (idle, and
  with a genuine active `hw:0,0` PCM stream open via `aplay`). With no mute
  GPIO wired (see above) and the DAI's own `.digital_mute` callback already
  auto-managing real mute state via the stream lifecycle, this control
  appears to have no live effect on this board -- don't rely on it.
- **`"Output Port Switch"`** (an `INTEGER`, range 0-5, ALSA control numid=8)
  genuinely **is** settable (confirmed: value persists across a fresh read,
  unlike "Mute Output") but empirically makes no observable difference to
  the jack-insertion pop -- tested live across all 6 values (0 through 5)
  with a real headphone plugged in, identical pop behavior every time. Likely
  an output-routing/channel-mapping selector unrelated to power/mute state.
- **Raw I2C register access exists** via three write-only (no `.show`/read
  handler at all, confirmed `EACCES` even as root) sysfs attributes on
  `/sys/bus/i2c/devices/3-0030/`: `write_reg_val` (backed by
  `sys_set_reg_val()` in the driver), `reg_val`, and `write_lr_flag`. This is
  a genuine low-level debug/bringup interface to the chip's own register map
  -- powerful, but **do not write to it blindly**: there is no datasheet
  available for this exact chip variant on this board, no readback path to
  verify effect, and a wrong register/value could plausibly cause a worse
  pop, an unexpected gain state, or other undocumented behavior. Only use
  this with the actual CS43131 register map in hand.
- **`cs43131_set_power`** is an **exported kernel symbol**
  (`__ksymtab_cs43131_set_power`), i.e. a kernel-to-kernel API other modules
  can call directly -- confirmed as the mechanism `x1600_hiby_r1_sound_card`
  uses. Not reachable from userspace at all (no matching `dev_attr_*` sysfs
  node), so this is purely an internal implementation detail, not a lever
  this app could ever pull directly even via raw sysfs.

### `sa_sound_switch.ko` ("SASS") -- headset presence detection

Despite the name, this is **detection only**, not a physical audio relay:
it's built entirely around the standard Android `switch_dev` framework
(`switch_dev_register`, `switch_set_state`, `dev_attr_switch_on`/
`dev_attr_switch_off`), with `get_switch_status()` exported for
`sa_earpods_adc` to consume. Detection itself is ADC-based, not GPIO-based on
this board (`sass_headset_det_gpio=-1`): channel 1, voltage range
2800-3300(mV, presumably), with a 200ms debounce (`sass_debounce_time=200`).
This is what backs the `/sys/devices/virtual/switch/headset/state` node
`headphone_status.c` already reads (`headphone_is_connected()`) -- confirming
that file's own doc comment about it being a standard Android-style
switch-class node.

The "balance" (balanced output), "lineout", and "balancelo" detection paths
this same module supports are all left fully unconfigured (`-1` across every
one of their own GPIO/ADC parameters) -- this board only ever detects a
plain headset.

### `sa_earpods_adc.ko` -- headset remote button detection

Depends on `soc_adc` and `sa_sound_switch` (confirmed via the module's own
`depends=` string), and only makes sense once a headset is actually present
(queries `sa_sound_switch`'s `get_switch_status()` first). Reads ADC channel
2 for an in-line remote's resistor-ladder buttons: play/pause at 0-10(mV?),
volume-up at 50-250, volume-down at 350-550, with press/hold/repeat timing
parameters matching a real held-button auto-repeat implementation. Registers
a genuine Linux input device (`earpods-adc/input0`) -- this is a **separate**
button source from `keyboard_adc_multifunc.ko`'s own physical hardware
buttons, and from what this session's earlier `hw_buttons.c` work covers;
whether this app currently reads headset-remote button presses at all is a
separate, not-yet-investigated question.

## Investigating the AVRCP playback-control bug: a dead end, and why

Real-device bug report: on a fresh boot, a connected Bluetooth headphone's
own play/pause/next/previous buttons didn't work. The suspected mechanism
was this device's own documented D-Bus split-brain condition (`S30dbus` and
`S80_bt_init` each starting an independent `dbus-daemon --system`, see
`bluetooth_control.h`'s own extensive history) possibly landing this app's
`bt_media_player.c` MediaPlayer1 registration on a different bus than
`bluetoothd` itself -- and, confirmed separately and independently of that
theory, a genuine bounded-retry-exhaustion bug in `bt_media_player_init()`
(fixed with a background retry thread, see `bt_media_player.c`'s own
comments).

Applying the same driver-archaeology approach used for the audio pop
investigation above turned up nothing further here, and it's worth recording
*why*, so a future session doesn't retread the same ground: `cywdhd.ko`
(the only module with "Bluetooth" anywhere in its own description) is the
**Wi-Fi SDIO driver** for the combo chip, confirmed via `strings`
(`dhd_bus_download_firmware`, `bcmsdh_reset`, `CYW43455 firmware and NVRAM
path`, etc.) -- its only Bluetooth-relevant surface is the `bt_reg_on`/
`bt_wake` GPIOs that physically power the BT half of the combo chip. That's
as far as any kernel module's responsibility goes. Firmware flashing
(`brcm_patchram_plus`, over UART, triggered by the `/etc/init.d/S80_bt_init`
script -- not a kernel module at all), `bluetoothd`/BlueZ, `dbus-daemon`,
and this app's own D-Bus client are **entirely userspace**, with no kernel
driver in between to inspect. Neither `/module_driver` nor `strings` against
any of its `.ko` files has anything more to offer for a bug that's
fundamentally about two `dbus-daemon` processes and D-Bus client/service
bus selection -- the fix space here is bounded to userspace-only measures
(the background retry already implemented, and -- deliberately not
attempted, see `bluetooth_control.h` -- any boot-time daemon consolidation).

## Investigating a new hardware issue: the general approach

This is the methodology that produced everything above, worth reusing for
future hardware-adjacent bugs:

1. **Read the loader script first** (`cat module_driver/<name>.sh`) -- the
   `insmod` parameters alone reveal GPIO assignments, ADC channels/ranges,
   and which features are actually wired up on **this specific board**
   (a parameter left at its sentinel value, usually `-1`, means that whole
   code path is dead on this hardware even if the driver supports it more
   generally -- e.g. the mute GPIO, or three of `sa_sound_switch`'s four
   detection channels).
2. **`strings` the `.ko` binary**, filtered for the concept you're chasing
   (`mute`, `power`, `pop`, `delay`, `gpio`, `dapm`, the sysfs attribute name
   if you already found one live on the device). This surfaces
   `MODULE_DESCRIPTION`/author/license, printk format strings (often
   genuinely descriptive, e.g. `"[SASS]switch_probe OK"`), parameter names,
   exported symbols (`__ksymtab_*`/`__kstrtab_*` pairs -- these are the
   kernel-to-kernel API surface), and sysfs attribute backing functions
   (`dev_attr_<name>` paired with the function that actually implements it).
3. **Cross-reference live on the device**: `amixer`/`amixer controls`/
   `amixer contents` for the ALSA-exposed surface, `find /sys -iname
   '*<keyword>*'` for anything sysfs-exposed outside ALSA, `dmesg | grep`
   for boot-time or runtime driver log lines, and `cat
   /sys/module/<name>/parameters/*` for the actual GPIO pin numbers/values a
   given driver ended up configured with (Ingenic GPIO naming: `P<bank><pin>`
   maps to global GPIO number `bank_index*32 + pin`, e.g. `PB02` = `32+2` =
   `34` -- confirmed directly against this exact pin in dmesg's own
   `"cs43131_power pin:34 output"` line).
4. **Before writing to anything discovered this way**, check whether it's
   genuinely settable at all: write, then read back **immediately, in the
   same shell invocation** (`amixer cset numid=N val; amixer cget numid=N`).
   A control that reports success on write but reverts on an immediate
   readback (as `"Mute Output"` does here) is not actually controllable from
   userspace on this driver -- don't build a fix around it.
5. **Prefer levers already proven safe over new ones.** This app's own
   pause-time pop fix (closing the PCM device, letting the kernel's already-
   correct stop sequence run) is safer and better-understood than any new
   mixer/GPIO/register poke would be -- when a new symptom looks related to
   one already fixed, check first whether it's reachable via the *same*,
   already-validated mechanism (e.g. a stream that's simply never been
   opened yet has never gone through that sequence even once) before
   reaching for something new.
6. **Never write to a raw/undocumented register interface blindly.** A
   write-only sysfs node with no readback and no datasheet (like this
   codec's `write_reg_val`) can only be used safely with the actual register
   map in hand -- guessing risks making the exact symptom being chased worse,
   or something unrelated and harder to detect.
