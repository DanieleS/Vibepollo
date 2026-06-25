# Vibepollo — Analisi di porting su Linux (CachyOS / Arch)

> Data: 2026-06-25
> Target: CachyOS (Arch-based), sia X11 che Wayland (KDE/Hyprland tipici su CachyOS).

## 1. Contesto: da dove si parte

Vibepollo è un fork di **Apollo**, a sua volta fork di **Sunshine** (host di game streaming
compatibile Moonlight/GameStream). Questo è il fatto più importante per il porting:

**La base Sunshine è nativamente cross-platform e Linux è una piattaforma di prima classe.**
Esistono già:

- `src/platform/linux/` completo (capture, audio, input, virtual display helper, mDNS).
- Packaging Linux funzionante: `packaging/linux/Arch/PKGBUILD`, unit systemd
  (`app-dev.lizardbyte.app.Sunshine.service.in`), Flatpak, AppImage, COPR/Fedora.
- Gating CMake per piattaforma (`cmake/compile_definitions/{windows,linux,macos}.cmake`).

Quindi il porting **non parte da zero**: il "motore" Sunshine compila e gira già su Arch. Il
lavoro reale riguarda esclusivamente lo **strato di feature aggiunte da Vibepollo**, che è
fortemente sbilanciato su Windows (`src/platform/windows/` contiene ~80 file, contro la manciata
di feature realmente nuove e cross-platform).

### Come Vibepollo isola il codice Windows

Il pattern usato in tutto il codice "common" è:

```cpp
#ifdef _WIN32
  // implementazione reale (es. display_helper_integration::apply(...))
#else
  // no-op / fallback (ritorna false, "[]", ecc.)
#endif
```

Vedi `src/display_helper_integration.h`: su non-Windows tutte le funzioni sono stub inline che
ritornano valori neutri. Conseguenza pratica: **il progetto compila su Linux anche con le feature
Windows presenti nei file common, ma quelle feature sono inerti.** Gli endpoint REST/UI di
Playnite e RTSS (`confighttp_playnite.cpp`, `confighttp_rtss.cpp`) sono compilati ovunque ma
chiamano backend `#ifdef _WIN32` vuoti su Linux.

---

## 2. Categoria A — Funziona già su Linux (zero o sforzo minimo)

Tutto ciò che è ereditato dal core Sunshine, più le feature Vibepollo che sono pure
frontend/HTTP/logica applicativa.

| Feature | Stato su Linux | Note |
|---|---|---|
| **Streaming classico Moonlight/GameStream** (RTSP, ENet, pairing) | ✅ Nativo | `nvhttp/rtsp/stream` sono cross-platform |
| **Cattura video** | ✅ Nativo | X11 (`x11grab`), Wayland (`wlgrab`), KMS/DRM (`kmsgrab`), KWin ScreenCast (`kwingrab`), XDG Desktop Portal (`portalgrab`) |
| **Encoding hardware** | ✅ Nativo | NVENC (CUDA), VAAPI (AMD/Intel), Vulkan video encode, software x264/x265 |
| **Cattura audio** | ✅ Nativo | PulseAudio / PipeWire (`platform/linux/audio.cpp`) |
| **Iniezione input** (tastiera/mouse/gamepad/touch) | ✅ Nativo | `inputtino` su uinput/libevdev — sostituisce ViGEm di Windows |
| **mDNS / discovery** | ✅ Nativo | Avahi (`platform/linux/publish.cpp`) |
| **UPnP port mapping** | ✅ Cross-platform | `upnp.cpp` |
| **Web UI ridisegnata + supporto mobile** | ✅ Cross-platform | Solo frontend Vue/TS, nessuna dipendenza OS |
| **Autenticazione a sessione + "remember me"** | ✅ Cross-platform | `http_auth.cpp` |
| **API Token Management (scoping per metodo)** | ✅ Cross-platform | `http_auth.cpp` / `confighttp.cpp` |
| **Notifiche di aggiornamento** | ✅ Cross-platform | `update.cpp` |
| **Session history / statistiche host** | ✅ Cross-platform | `session_history*`, `host_stats` ha impl. Linux dedicata |
| **System tray** | ✅ Linux | via `libayatana-appindicator` + `libnotify` |

**Sforzo stimato categoria A:** sostanzialmente nullo. Serve solo *verificare* che il build Arch
attuale produca un binario funzionante con queste feature attive (il PKGBUILD esistente è il punto
di partenza). Questo è già l'80% del valore percepito da un utente Moonlight.

---

## 3. Categoria B — Da riadattare (sforzo medio/alto, ma fattibile)

Feature il cui *concetto* è portabile ma la cui *implementazione* attuale è Windows-only.

### B1. WebRTC Browser Streaming (`/webrtc`)
- **Stato:** il core C++ (`webrtc_stream.cpp`) e tutto il lato browser (Vue/TS) sono
  cross-platform e usano una C-API verso `libwebrtc`. L'opzione CMake `SUNSHINE_ENABLE_WEBRTC` è
  però marcata "Windows only" e di default OFF; lo script di build di libwebrtc è PowerShell
  (`scripts/build_mingw_webrtc.ps1`).
- **Cosa manca su Linux:**
  1. Buildare `libwebrtc` per Linux e plumbarlo in `webrtc.cmake` (il ramo non-Windows esiste già
     come `WEBRTC_ROOT` ma non è testato).
  2. La preparazione del display attorno alla sessione WebRTC
     (`display_helper_integration::apply`, `prepare_virtual_display_for_webrtc_session`) è
     Windows-only no-op → su Linux la sessione parte ma **senza** automazione display/virtual
     display (vedi B2/C2).
- **Sforzo:** medio-alto. La parte difficile è la toolchain di `libwebrtc`, non il codice Vibepollo.
  Fattibile.

### B2. Display Setting Automation (cambio risoluzione/refresh, restore layout)
- **Stato:** logica molto sofisticata ma interamente Windows (`display_helper_v2/`, CCD API,
  scheduled task, IPC helper, watchdog). Su Linux è no-op.
- **Equivalente Linux:** esiste a livello concettuale. `libdisplaydevice` (già dipendenza del
  progetto) ha backend; il cambio modeset si farebbe via **xrandr** (X11), **wlr-randr**/protocollo
  `wlr-output-management` (wlroots/Hyprland) o **KScreen/KWin DBus** (KDE su CachyOS).
- **Sforzo:** alto e **frammentato per compositor**. Su Wayland non esiste un'API unica: KDE,
  Hyprland e GNOME richiedono percorsi diversi. La robustezza "anti-stuck / restore dopo crash"
  tipica di Vibepollo andrebbe riscritta da capo. Realistico mirare prima a X11/KDE.

### B3. Hotkey globali / rilevamento finestra in foreground
- `hotkey_manager.cpp`, `foreground_app.cpp` sono Win32.
- **Equivalente Linux:** X11 (XGrabKey / `_NET_ACTIVE_WINDOW`) fattibile; su Wayland le hotkey
  globali e l'identità della finestra attiva sono volutamente ristrette (serve integrazione per
  compositor o portal globalshortcuts).
- **Sforzo:** medio su X11, alto/parziale su Wayland.

---

## 4. Categoria C — Nessuna alternativa Linux compatibile (vendor/OS-locked)

Feature legate a software o API che **non esistono su Linux**. Qui non si "porta", al massimo si
trova un sostituto *diverso* (e spesso non c'è).

| Feature Vibepollo | Perché è bloccata | Alternativa Linux |
|---|---|---|
| **Native Virtualized Display (SudoVDA)** | Driver kernel Windows (`third-party/sudovda`, IDD) | Nessun equivalente diretto. Concettualmente si può creare un output virtuale (dummy EDID, output virtuale wlroots, monitor headless KMS), ma è tutt'altra implementazione. La parte "display fittizio 240 Hz per frame-gen" non si mappa. |
| **Windows Graphics Capture in service mode** | API WGC + sessione di servizio Windows | Non serve: su Linux la cattura è già nativa (KMS/Wayland/portal). Feature di fatto *moot*. |
| **Playnite Integration** | Playnite è app .NET **solo Windows**; IPC via named pipe + plugin | Nessuna. Su Linux l'analogo sarebbe integrare **Lutris / Heroic / Steam**: integrazione completamente nuova da progettare. |
| **RTSS & frame pacing** | RivaTuner Statistics Server è solo Windows | Nessuna integrazione equivalente (concettualmente MangoHud/libstrangle, ma non è la stessa cosa e non c'è API). |
| **Frame-Generated Capture Fixes** | Dipendono da WGC + RTSS + display ad alto refresh | Non applicabile (stack Windows-specifico). |
| **Lossless Scaling integration** | `LosslessScaling.exe` è app Steam **solo Windows** | Nessuna (il software non esiste su Linux). |
| **NVIDIA Smooth Motion / NVIDIA Control Panel** | NVAPI (`nvprefs/`, `third-party/nvapi`) è **solo Windows** | Nessuna: NVAPI non esiste su Linux. Lato driver Linux non c'è equivalente esposto. |
| **RTX HDR / TrueHDR** | NVAPI + shim DLL MSVC (`vibeshine_truehdr.dll`) | Nessuna. |
| **Frame limiter (NVCP/RTSS)** | `frame_limiter_nvcp` su NVAPI/RTSS | Nessuna integrazione equivalente. |
| **ViGEm (gamepad emulation)** | Driver Windows | **Già sostituito** da `inputtino` (uinput) — vedi Categoria A. |

**Punto chiave:** quasi tutto il "valore aggiunto premium" di Vibepollo rispetto a Sunshine vanilla
(virtual display SudoVDA, frame-gen fixes, Lossless Scaling, Smooth Motion, RTX HDR, Playnite, RTSS)
è costruito su **NVAPI / driver kernel Windows / app Windows-only**. Questo strato **non è portabile**:
si può solo decidere se reimplementare un sottoinsieme con tecnologie Linux diverse (grosso lavoro,
risultato non equivalente) oppure lasciarlo disabilitato.

---

## 5. Quadro di sintesi (effort vs valore)

```
                    Valore utente
                         ^
   Streaming core    A   |   ●  (già funziona — priorità di verifica)
   WebUI/Auth/Token  A   |   ●
   WebRTC            B   |        ◐  (medio-alto: toolchain libwebrtc)
   Display automation B  |        ◐  (alto, per-compositor)
   Virtual display   C   |             ○  (no equivalente diretto)
   Playnite/LS/RTSS/ C   |             ○  (Windows-locked, no port)
   Smooth Motion/HDR
                         +------------------------------> Sforzo
```

- **A (verde):** ~0 sforzo, è già lì. È un Sunshine/Apollo Linux pienamente funzionante.
- **B (giallo):** lavoro reale di ingegneria ma fattibile; WebRTC e display automation X11/KDE.
- **C (rosso):** non si porta. O si rinuncia, o si reinventa con stack Linux (progetto a parte).

---

## 6. Raccomandazione operativa

1. **Fase 0 — Validare la base.** Compilare il PKGBUILD esistente su CachyOS, verificare streaming
   classico Moonlight + WebUI + encoding (NVENC su Nvidia / VAAPI su AMD-Intel) + input. Questo da
   solo è un host pienamente usabile.
2. **Fase 1 — WebRTC.** Buildare `libwebrtc` per Linux e abilitare `SUNSHINE_ENABLE_WEBRTC`. È la
   feature Vibepollo "di punta" che è realmente cross-platform.
3. **Fase 2 — Display automation (X11/KDE prima).** Reimplementare cambio risoluzione/refresh e
   restore via KScreen/xrandr. Wayland generico è una sotto-fase a sé.
4. **Esplicitamente fuori scope** (almeno inizialmente): SudoVDA, Playnite, Lossless Scaling,
   Smooth Motion, RTX HDR, RTSS. Vanno presentati all'utente come "feature Windows-only".

L'effort dominante NON è il porting del codice Sunshine (già fatto), ma: (a) toolchain libwebrtc
Linux, (b) riscrittura per-compositor della gestione display, (c) decidere quali feature C
abbandonare. La maggior parte del codice in `src/platform/windows/` semplicemente **non verrà
portata**.

---

## 7. Domande aperte (per indirizzare meglio l'analisi/implementazione)

1. **GPU target principale su CachyOS?** Nvidia (NVENC/CUDA) o AMD/Intel (VAAPI/Vulkan)? Cambia
   priorità e quali "feature C Nvidia" pesano davvero.
2. **X11 o Wayland?** È la variabile che più impatta display automation, hotkey e foreground app.
   CachyOS spinge Wayland (KDE/Hyprland), che è il caso più difficile.
3. **Obiettivo:** vuoi un host Vibepollo *funzionale* su Linux (Categoria A+B) o puntare a
   replicare anche le feature premium Nvidia (Categoria C, di fatto un nuovo progetto)?
4. **WebRTC è un must-have?** Determina se investire nella toolchain `libwebrtc` Linux fin da
   subito.
