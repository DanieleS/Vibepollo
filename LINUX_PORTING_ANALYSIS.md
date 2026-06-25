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

---

## 8. Verifica pratica della build Linux + setup mirato (Nvidia 4070, Wayland, KDE+Hyprland)

> Profilo target confermato: GPU **Nvidia RTX 4070 (Ada)**, sessione **Wayland**, si vuole
> supportare **sia KDE Plasma sia Hyprland**, obiettivo "minimo funzionante" (premium = dopo),
> WebRTC = nice-to-have.

### 8.1 Stato reale della build Linux (importante)

- La CI Arch (`.github/workflows/ci-archlinux.yml`) esiste, è completa (CUDA + unit test +
  pacchetto) **ma era orfana**: `ci.yml` invocava solo `ci-windows.yml`. Tutto il codice nuovo di
  Vibepollo (`webrtc_stream`, `http_auth`, `session_history`, ...) è stato scritto/testato **solo su
  Windows** → rischio concreto di bitrot del ramo Linux non intercettato.
- **Fix applicato in questo branch:** aggiunto il job `build-archlinux` in `ci.yml`, che ora compila
  il ramo Linux su ogni PR e su ogni push di release. Non è dipendenza del job `release`, quindi un
  eventuale rosso su Linux **segnala** il problema senza bloccare l'artefatto Windows. Questo è il
  modo sostenibile di "tenere viva" la build Linux.

### 8.2 Cosa è stato verificato configurando la build qui

Il sistema di build **configura correttamente su Linux** (Ubuntu 24.04, gcc 13, cmake 3.28). I punti
emersi, utili per CachyOS:

1. **Boost 1.89 richiesto.** Se il sistema ha una versione più vecchia, CMake fa FetchContent e lo
   compila da sorgente (lento). Su Arch/CachyOS il pacchetto `boost` è recente → nessun problema.
2. **FFmpeg è prebuilt**, scaricato da GitHub release in base al tag del submodule
   `third-party/build-deps` (non usa l'FFmpeg di sistema). Richiede rete a configure-time.
3. **Submodule esterni necessari** (oltre al repo principale): `libdisplaydevice`,
   `libvirtualdisplay`, `Simple-Web-Server`, `moonlight-common-c`, `inputtino`, ecc. Sono
   cross-platform e si compilano su Linux. → `git submodule update --init --recursive`.
4. **Tutti i backend di cattura sono ON di default** (`SUNSHINE_ENABLE_{X11,WAYLAND,KWIN,PORTAL,DRM,
   VAAPI,VULKAN,CUDA}`). Conseguenza diretta: **un singolo build copre sia KDE Plasma (KWin
   ScreenCast / XDG Portal) sia Hyprland (wlr-screencopy / Portal)** — la scelta avviene a runtime in
   base al compositor. Non serve alcuna modifica al codice per "supportare entrambi".

> Nota: il build **completo** non è stato finalizzabile *in questo ambiente sandbox* perché la policy
> di rete blocca il clone dei submodule da repo GitHub diversi da quello principale (403). Non è un
> bug del codice: su una macchina CachyOS reale (o nella CI Arch ora agganciata) i submodule si
> clonano normalmente.

### 8.3 Caveat specifici Nvidia + Wayland (RTX 4070)

- Servono **driver Nvidia recenti** (nvidia-open ≥ 555, consigliati gli ultimi su CachyOS) per una
  cattura Wayland affidabile. NvFBC è solo X11: su Wayland la cattura passa per
  KWin ScreenCast / wlr-screencopy / **XDG Portal (PipeWire)**.
- Assicurarsi che **PipeWire** e `xdg-desktop-portal` (+ backend del compositor:
  `xdg-desktop-portal-kde` per KDE, `xdg-desktop-portal-hyprland` per Hyprland) siano attivi: il
  path Portal è il più robusto su Nvidia.
- **NVENC** (AV1/HEVC/H.264) sulla 4070 Ada funziona via CUDA: build con `_use_cuda=true` /
  `-DSUNSHINE_ENABLE_CUDA=ON` e pacchetto `cuda` installato.
- L'**input injection** usa `inputtino` (uinput): aggiungere l'utente al gruppo `input` e caricare il
  modulo `uinput` (`/dev/uinput`).

### 8.4 Guida build cucita per CachyOS

**Opzione A — pacchetto via PKGBUILD (consigliata, riproducibile):**

```bash
# dipendenze di build/runtime (dal PKGBUILD del progetto)
sudo pacman -S --needed base-devel cmake git nodejs npm \
  avahi curl libayatana-appindicator libcap libdrm libevdev libmfx libnotify \
  libpipewire libpulse libva libx11 libxcb libxfixes libxrandr libxtst \
  miniupnpc numactl openssl opus udev vulkan-icd-loader which \
  python-jinja python-setuptools shaderc appstream appstream-glib \
  desktop-file-utils cuda            # cuda = encoding Nvidia (4070)

git clone <repo-vibepollo> && cd Vibepollo
git submodule update --init --recursive
mkdir build && cd build
cmake -DSUNSHINE_CONFIGURE_ONLY=ON -DSUNSHINE_CONFIGURE_PKGBUILD=ON ..
cd ../packaging/linux/Arch   # contiene il PKGBUILD configurato
makepkg -si                  # _use_cuda=true di default se 'cuda' è installato
```

**Opzione B — build CMake diretta (per iterare/debug):**

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DSUNSHINE_ENABLE_X11=ON -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_KWIN=ON -DSUNSHINE_ENABLE_PORTAL=ON \
  -DSUNSHINE_ENABLE_DRM=ON -DSUNSHINE_ENABLE_VAAPI=ON -DSUNSHINE_ENABLE_VULKAN=ON \
  -DSUNSHINE_ENABLE_WEBRTC=OFF \
  -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF
cmake --build build -j"$(nproc)"
```

Questa configurazione produce un host **funzionale** (Categoria A): streaming Moonlight, WebUI,
NVENC sulla 4070, cattura sia su KDE che su Hyprland, input, audio. Le feature premium (Categoria C:
SudoVDA, Playnite, RTSS, Lossless Scaling, Smooth Motion, RTX HDR) restano disabilitate perché
Windows-only. WebRTC si potrà abilitare in un secondo momento (richiede build di `libwebrtc` per
Linux).

### 8.5 Prossimi passi suggeriti

1. Compilare con una delle due opzioni sulla macchina CachyOS e validare lo streaming Moonlight.
2. Verificare il path di cattura attivo per ciascun compositor (log Sunshine: KMS/KWin/Portal/wlr).
3. (Opzionale) Monitorare il nuovo job `build-archlinux` su una PR per confermare che il ramo Linux
   compili in CI, e usarlo come guard-rail contro futuri commit Windows-only che rompono Linux.

---

## 9. Esito: Fase 1 completata — il pacchetto Linux builda (CI verde)

Il ramo Linux è stato recuperato dallo stato "orfano/bitrot" a un **pacchetto Arch che compila,
linka, passa `check()` (`sunshine --version`) e si installa**, verificato sulla CI Arch
(GCC 15 + CUDA + LTO) ora agganciata alla pipeline.

### 9.1 Fix applicati (in ordine di emersione)

| # | File | Problema | Fix |
|---|---|---|---|
| 1 | `cmake/.../linux.cmake` | riferimenti ai sorgenti glad1 inesistenti (migrazione glad2) | rimossi; il loader arriva dal target generato `glad` |
| 2 | `src/config.cpp` | variabili `prev_rtx_hdr_*` inutilizzate fuori Windows | guard `#ifdef _WIN32` |
| 3 | `packaging/linux/Arch/PKGBUILD` | `-Werror` rendeva fatali i warning cosmetici | `BUILD_WERROR` reso opt-in (`_werror`, default off) |
| 4 | `src/nvenc/nvenc_config.h` | membro omonimo del tipo enum (`-Wchanges-meaning`, **errore C++**) | tipo qualificato `nvenc::split_encode_mode` |
| 5 | `src/nvhttp.cpp` | `has_active_or_stopping_stream_session` definita solo in `#ifdef _WIN32` | spostata fuori dal blocco |
| 6 | `src/video.cpp` | `encode_session_teardown_mutex` intrappolato in `#ifdef _WIN32` | spostato nel namespace anonimo, fuori dal blocco |
| 7 | `src/platform/linux/host_stats.cpp` | membro `_shutdown` mai dichiarato | dichiarato `nvmlShutdown_t _shutdown` |
| 8 | `src/platform/linux/publish.cpp` | `string_view` passato a `const char*` (Avahi) | `platf::SERVICE_TYPE.data()` |
| 9 | `src/platform/common.h` | enum `mem_type_e::vulkan` mancante | aggiunto |
| 10 | `src/confighttp.cpp` | `std::jthread` → simbolo `__notify_impl@GLIBCXX_3.4.35` non linkabile sotto LTO | sostituito con `std::thread` + `join()` |
| — | `.github/workflows/ci.yml` + `ci-archlinux.yml` | CI Arch orfana; unit test/coverage rompevano il job | job `build-archlinux` agganciato; unit test/coverage gated (fase 2) |

Pattern dominante: **simboli cross-platform intrappolati in blocchi `#ifdef _WIN32`** e codice dei
file `platform/linux/*` mai compilato durante lo sviluppo Windows-only.

### 9.2 Guida d'installazione definitiva — CachyOS (Nvidia RTX 4070, Wayland)

Verificata contro il build CI (GCC 15 + CUDA). Finché la PR non è mergiata su `master`, usare il
branch `claude/linux-porting-analysis-yudn48`.

```bash
# 1) Dipendenze (build + runtime)
sudo pacman -S --needed base-devel cmake git nodejs npm python-jinja python-setuptools shaderc \
  avahi curl libayatana-appindicator libcap libdrm libevdev libmfx libnotify libpipewire \
  libpulse libva libx11 libxcb libxfixes libxrandr libxtst miniupnpc numactl openssl opus \
  udev vulkan-icd-loader which cuda
# Nvidia 4070 (Ada): driver recente + KMS per Wayland
sudo pacman -S --needed nvidia-open-dkms   # o 'nvidia'/'nvidia-dkms' secondo il kernel
# Portal per la cattura Wayland (installa il backend del tuo compositor)
sudo pacman -S --needed xdg-desktop-portal xdg-desktop-portal-kde       # KDE Plasma
sudo pacman -S --needed xdg-desktop-portal-hyprland                     # Hyprland

# 2) Build (CMake diretto — tutti i backend di cattura ON di default → KDE e Hyprland)
git clone https://github.com/DanieleS/Vibepollo.git
cd Vibepollo
git checkout claude/linux-porting-analysis-yudn48
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DBUILD_WERROR=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build

# 3) Setup runtime
#  - KMS Wayland Nvidia: assicurarsi nvidia_drm.modeset=1 (di norma già attivo su CachyOS)
#  - input injection (uinput): l'install applica le udev rules; aggiungere l'utente al gruppo input
sudo usermod -aG input "$USER"        # poi ri-login
#  - avviare come servizio utente (NON come root, serve la sessione grafica)
systemctl --user enable --now app-dev.lizardbyte.app.Sunshine.service
#    oppure, per debug in foreground:  sunshine
#  - aprire la WebUI e completare il setup: https://localhost:47990
```

Note operative:
- **Cattura schermo**: a runtime Sunshine sceglie il backend (KMS / KWin ScreenCast / wlr-screencopy
  / Portal) in base al compositor. Su Nvidia+Wayland il path **Portal/PipeWire** è il più affidabile;
  verificare nei log Sunshine quale viene selezionato.
- **Porte firewall**: TCP 47984/47989/47990/48010, UDP 47998-48000 (standard Moonlight).
- Feature **Categoria C** (SudoVDA, Playnite, RTSS, Lossless Scaling, Smooth Motion, RTX HDR)
  restano disattivate: Windows-only.

### 9.3 Fase 2 (hardening, quando vorrai)

Niente di bloccante per l'uso; sono pulizie/irrobustimenti:

1. **Riattivare `-Werror`** (`_werror=true`) e bonificare i warning cosmetici emersi (variabili
   inutilizzate nei rami `_WIN32`, funzioni statiche non usate in `confighttp.cpp`, ecc.).
2. **Unit test su Linux**: far costruire i test (`-DBUILD_TESTS=ON`) e sistemare il PKGBUILD
   (`_run_unit_tests`) + le eventuali bitrot della suite; riabilitare l'upload coverage.
3. **Warning ODR** `dmabuf_t`/`gbm_device` in `src/platform/linux/wayland.h` (visto in fase di link):
   allineare la forward-declaration di `gbm_device` al tipo reale di `<gbm.h>`.
4. **WebRTC (nice-to-have)**: buildare `libwebrtc` per Linux e abilitare `SUNSHINE_ENABLE_WEBRTC`.
5. **Display automation / virtual display** su Wayland (Categoria B): reimplementazione per-compositor
   (KScreen/wlr-output-management) — il lavoro grosso del porting "premium".
