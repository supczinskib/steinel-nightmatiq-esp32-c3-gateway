# Bramka Steinel NightmatIQ Plus dla ESP32-C3

[English](README.md) · [Deutsch](README_DE.md)

> **Samodzielna bramka Bluetooth Mesh dla ESP32-C3 zapewniająca lokalne sterowanie Steinel NightmatIQ Plus, diagnostykę, aktualizacje firmware i integrację z Home Assistant.**

```text
Steinel NightmatIQ Plus <-> Bluetooth Mesh <-> bramka ESP32-C3 -> Home Assistant
```

Społecznościowy firmware ESPHome zmieniający ESP32-C3 Super Mini w dedykowaną, lokalną bramkę Steinel NightmatIQ Plus. Importuje istniejącą konfigurację sieci Steinel, komunikuje się bezpośrednio przez Bluetooth Mesh, udostępnia chroniony hasłem interfejs przeglądarkowy oraz przekazuje do Home Assistant encje sterujące, pomiarowe, identyfikacyjne i diagnostyczne.

Autor i opiekun projektu: **Bartosz Supcziński** — <bartek@env.pl>

## Dlaczego powstał ten projekt

NightmatIQ Plus komunikuje się przez Bluetooth Mesh, natomiast Home Assistant korzysta z sieci IP. ESP32-C3 łączy te dwa środowiska: dołącza do istniejącej instalacji Mesh, wymienia polecenia i informacje o stanie bezpośrednio z sensorem oraz publikuje je przez ESPHome. Steinel Cloud służy podczas konfiguracji do importu ustawień sieci; późniejsza praca odbywa się lokalnie.

## Zrzuty ekranu

### ESP32-C3 Super Mini

![ESP32-C3 Super Mini](docs/images/esp32-c3-super-mini.jpg)

### Lokalny interfejs WWW

Wbudowana strona umożliwia konfigurację, sterowanie, diagnostykę i aktualizację firmware z przeglądarki.

![Lokalny interfejs NightmatIQ](docs/images/nightmatiq-web-interface.png)

### Urządzenie w Home Assistant

Standardowa integracja ESPHome udostępnia NightmatIQ bezpośrednio jako jedno urządzenie Home Assistant.

![Urządzenie NightmatIQ w Home Assistant](docs/images/home-assistant-device.png)

### Opcjonalne okno sterowania Home Assistant

Opcjonalny moduł interfejsu łączy stan sensora, natężenie oświetlenia, tryb pracy i próg zmierzchowy w jednym zwartym oknie.

![Okno sterowania NightmatIQ w Home Assistant](docs/images/home-assistant-control.png)

## Co zapewnia projekt

### Lokalna integracja Bluetooth Mesh

- Import kopii sieci Steinel przy użyciu konta podanego w przeglądarce.
- Odtworzenie klucza sieciowego, klucza aplikacji, IV Index i danych węzła NightmatIQ.
- Bezpośrednia komunikacja z NightmatIQ przez Bluetooth Mesh.
- Odczyt rzeczywistego stanu wyjścia, natężenia oświetlenia, progu zmierzchowego, wersji firmware, rewizji sprzętu i identyfikacji produktu.
- Sterowanie trybami `Auto`, `Zawsze włączone` i `Zawsze wyłączone`.
- Zmiana progu zmierzchowego w zakresie od `1` do `1500 lx`.

### Odporna obsługa adresów i sesji

- Wybór adresu Mesh bramki z niezajętej części zakresu provisionera.
- Automatyczne odzyskiwanie komunikacji, gdy urządzenia Mesh odrzucają wcześniej używany adres źródłowy.
- Zapamiętanie pierwszego potwierdzonego adresu źródłowego, aby późniejszy restart lub chwilowa niedostępność sensora nie powodowały niepotrzebnych zmian.
- Zachowanie ustawień Mesh po zwykłym restarcie i aktualizacji OTA.
- Ograniczone ponowienia i kontrolowane restarty podczas przełączania chmury i Bluetooth.

### Interfejs WWW urządzenia

- Konfiguracja NightmatIQ z użyciem Steinel Cloud.
- Sterowanie i ręczne odświeżanie stanu.
- Zainstalowana konfiguracja i rozszerzona diagnostyka.
- RSSI Mesh oraz liczniki odpowiedzi.
- Chroniona hasłem aktualizacja OTA z przeglądarki.
- Panel **Gateway administration** obejmujący aktualizację firmware, zarządzanie hasłem administratora i pełny reset fabryczny.

### Integracja z Home Assistant

Standardowe API ESPHome publikuje:

- rzeczywisty stan wyjścia sensora;
- zmierzone natężenie oświetlenia;
- tryb pracy;
- próg zmierzchowy;
- gotowość i stan Bluetooth Mesh;
- siłę sygnału;
- wersję zainstalowanego firmware i rewizję sprzętu;
- producenta, Company ID i Product ID;
- przycisk ręcznego odświeżenia.

Home Assistant wyświetla wszystkie publikowane encje w jednym urządzeniu o nazwie **Steinel NightmatIQ Plus**.

## Sprzęt i kompatybilność

### Wymagany sprzęt

- ESP32-C3 Super Mini z 4 MB pamięci flash;
- natywny port USB/JTAG do pierwszej instalacji lub odzyskiwania;
- sieć Wi-Fi 2,4 GHz;
- instalacja Steinel NightmatIQ Plus widoczna na koncie Steinel.

Interfejs USB zwykle pojawia się jako urządzenie Espressif USB JTAG/serial (`303a:1001`) oraz `/dev/ttyACM*` w systemie Linux.

### Obsługiwana platforma

Firmware jest przeznaczony dla ESP32-C3 i ESP-IDF. Rozszerzone funkcje Bluetooth 5 są wyłączone, ponieważ Bluetooth Mesh korzysta ze ścieżki reklamowej BLE 4.2. Konfiguracja została dopasowana do ograniczonej pamięci RAM ESP32-C3.

## Bezpieczeństwo

- Fabryczne konto administratora to `admin` z hasłem `12345678`; zmień je na stronie urządzenia natychmiast po połączeniu z Wi-Fi.
- Hasło administratora chroni lokalną stronę i aktualizacje firmware oraz jest zapisywane w NVS ESP32.
- Punkt dostępowy do konfiguracji używa fabrycznego hasła `12345678`.
- Dane konta Steinel wpisane na stronie konfiguracji są używane do wymaganych połączeń HTTPS i nigdy nie są zapisywane przez bramkę.
- Lokalny interfejs WWW używa uwierzytelniania HTTP Digest. Chroni ono dostęp, ale nie szyfruje lokalnego ruchu HTTP.
- Domyślna konfiguracja natywnego API ESPHome nie używa klucza szyfrowania.
- Konfigurację i aktualizacje wykonuj tylko w zaufanej sieci LAN lub odseparowanej sieci IoT.
- Nie zatwierdzaj w Git prawdziwego `secrets.yaml`, kluczy prywatnych, przechwyconych pakietów ani kopii sieci Steinel.

## Struktura repozytorium

| Ścieżka | Przeznaczenie |
|---|---|
| `esphome/nightmatiq-c3.yaml` | Główna konfiguracja firmware ESPHome |
| `esphome/components/nightmatiq_mesh/` | Komponent Bluetooth Mesh, Steinel Cloud i lokalnego WWW |
| `scripts/` | Instalacja, walidacja, USB i OTA |
| `home-assistant/` | Opcjonalny pakiet i zwarte okno sterowania Home Assistant |
| `docs/images/` | Publiczne obrazy README |

## Instalacja gotowego firmware

Zalecana pierwsza instalacja nie wymaga kompilowania ESPHome:

1. Pobierz najnowszy plik `steinel-nightmatiq-esp32-c3-gateway-vX.Y.Z-factory.bin` z [wydań GitHub](https://github.com/supczinskib/steinel-nightmatiq-esp32-c3-gateway/releases/latest).
2. Otwórz [ESPHome Web](https://web.esphome.io/) w przeglądarce obsługującej WebSerial i podłącz ESP32-C3 przez USB.
3. Wybierz płytkę, użyj **Install** i wskaż pobrany plik `-factory.bin`.
4. Po instalacji przejdź do sekcji **Połączenie z Wi-Fi** i **Połączenie z NightmatIQ** poniżej.

ESPHome Web przetwarza plik lokalnie. Obraz `-factory.bin` służy do nowej płytki lub odzyskiwania przez USB; późniejsze aktualizacje z przeglądarki używają obrazu `-ota.bin`.

## Budowanie ze źródeł

## Wymagania

- komputer z systemem Linux lub macOS;
- Python 3 i obsługiwane środowisko ESPHome;
- dostęp USB przy pierwszej instalacji;
- dostęp sieciowy do ESP32-C3 i Steinel Cloud podczas pierwszej konfiguracji;
- Home Assistant jest opcjonalny.

Dostarczony instalator tworzy odizolowane, powtarzalne środowisko z niezmodyfikowanym ESPHome `2026.7.3`. Do zainstalowanego pakietu ESPHome nie jest nakładany żaden patch.

## 1. Pobranie i przygotowanie projektu

Sklonuj lub pobierz repozytorium, przejdź do jego katalogu i zainstaluj przypięte środowisko:

```bash
sudo bash scripts/01_install_esphome.sh
```

## 2. Walidacja

```bash
bash scripts/03_validate_all.sh
```

Polecenie wykonuje kontrolę repozytorium i sprawdza konfigurację ESPHome.

## 3. Pierwsza instalacja lub odzyskiwanie przez USB

Podłącz ESP32-C3 i w miarę możliwości użyj stabilnej ścieżki `/dev/serial/by-id/`:

```bash
sudo bash scripts/09_upload_usb.sh /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00
```

Jeżeli płytka nie ma dowiązania `by-id`, użyj wykrytego portu ACM:

```bash
sudo bash scripts/09_upload_usb.sh /dev/ttyACM0
```

Ten sam skompilowany obraz można zainstalować na każdej obsługiwanej płytce ESP32-C3. Pierwsza instalacja przez USB przygotowuje urządzenie do kolejnych aktualizacji z przeglądarki, dlatego później zwykle nie trzeba używać przycisku BOOT.

## 4. Połączenie z Wi-Fi

1. Połącz się z punktem dostępowym `nightmatiq-gateway-XXXXXX`, używając hasła `12345678`.
2. W portalu konfiguracji wybierz docelową sieć Wi-Fi 2,4 GHz i wpisz jej hasło.
3. Poczekaj, aż bramka uruchomi się ponownie i połączy z wybraną siecią.
4. Otwórz adres przydzielony przez router albo nazwę urządzenia zakończoną `.local`.

Konfiguracja Wi-Fi jest zapisywana przez urządzenie i pozostaje po aktualizacji firmware.

## 5. Połączenie z NightmatIQ

1. Otwórz adres bramki w przeglądarce.
2. Zaloguj się jako `admin`, używając fabrycznego hasła `12345678`.
3. W panelu **Gateway administration** zmień hasło w widocznej sekcji **Administrator access**. To samo nowe hasło będzie zatwierdzało kolejne aktualizacje firmware.
4. Po automatycznym restarcie zaloguj się ponownie.
5. Wpisz dane konta Steinel Cloud i pobierz listę sieci.
6. Wybierz sieć zawierającą NightmatIQ.
7. Zainstaluj konfigurację i poczekaj na restart bramki.

Adres węzła NightmatIQ i IV Index zwykle mogą zostać wybrane automatycznie z kopii sieci. Dane logowania pozostają tylko w formularzu przeglądarki na czas żądań konfiguracyjnych.

## 6. Aktualizacja przez Wi-Fi

Po otwarciu strony bramka sprawdza najnowsze stabilne wydanie GitHub; **CHECK FOR UPDATES** powtarza sprawdzenie ręcznie. Gdy dostępna jest nowsza wersja, **DOWNLOAD AND INSTALL** pobiera ją przez HTTPS, sprawdza rozmiar i sumę SHA-256, instaluje oraz restartuje bramkę. Nieudana aktualizacja pozostawia dotychczasowy firmware aktywny.

Instalacja ręczna pozostaje dostępna w sekcji **Manual firmware file**. Używaj wyłącznie pliku wydania zakończonego `-ota.bin`; `-factory.bin` służy tylko do pierwszej instalacji przez USB. Użytkownik nie otrzymuje ani nie musi znać osobnego „hasła OTA”.

**Factory reset** usuwa ustawienia Wi-Fi, administratora i NightmatIQ Mesh, a następnie przywraca fabryczne konto i punkt dostępowy bez zmiany zainstalowanej wersji firmware.

Z wiersza poleceń — podaj hasło administratora, gdy skrypt o nie poprosi:

```bash
bash scripts/05_upload_ota.sh ADRES_IP_LUB_NAZWA_HOSTA
```

## 7. Integracja z Home Assistant

Home Assistant zwykle wykrywa urządzenie automatycznie przez ESPHome. Jeżeli tak się nie stanie:

1. Otwórz **Ustawienia → Urządzenia oraz usługi**.
2. Dodaj integrację **ESPHome**.
3. Podaj adres IP lub nazwę hosta bramki.
4. Przypisz **Steinel NightmatIQ Plus** do właściwego obszaru.

Wszystkie encje sterujące i diagnostyczne są bezpośrednio przypisane do tego urządzenia.

## 8. Opcjonalne zwarte okno Home Assistant

Standardowa integracja ESPHome udostępnia wszystkie encje i elementy sterujące. Pliki w `home-assistant/` dodają zwarty kafelek obszaru i pokazane wyżej okno sterowania.

1. Skopiuj `steinel-nightmatiq-package.yaml` do katalogu pakietów Home Assistant.
2. Skopiuj `steinel-nightmatiq-popup.js` do `/config/www/`.
3. Dodaj `/local/steinel-nightmatiq-popup.js?v=100` jako moduł JavaScript w zasobach panelu.
4. Przeładuj konfigurację pakietów i odśwież pamięć podręczną przeglądarki.

Pliki używają domyślnych identyfikatorów encji tworzonych przy pierwszej instalacji. Jeżeli Home Assistant dopisał `_2` lub inny sufiks, zmień cztery identyfikatory na początku pliku JavaScript i odpowiadające im identyfikatory w pliku pakietu YAML.

Moduł dostosowuje automatycznie wygenerowany kafelek obszaru i okno szczegółów Home Assistant. Ponieważ strategia obszaru jest częścią interfejsu Home Assistant, przyszła wersja frontendu może wymagać aktualizacji opcjonalnego modułu.

## Wiele bramek

Podczas importu sieci każda bramka wyznacza politykę adresu Mesh na podstawie wybranej instalacji i własnej tożsamości sprzętowej. Ten sam firmware można dzięki temu skonfigurować dla różnych płytek ESP32-C3 i instalacji NightmatIQ.

Sufiks MAC w nazwie urządzenia jest domyślnie włączony, dlatego wiele bramek otrzymuje unikalne nazwy hostów i punktów dostępowych. Na każdej bramce ustaw osobne hasło administratora.

## Awaryjny punkt dostępowy

Jeżeli skonfigurowana sieć Wi-Fi jest niedostępna przez 90 sekund, bramka ponownie uruchamia chroniony punkt dostępowy. Połącz się z nim fabrycznym hasłem AP `12345678` i zmień konfigurację Wi-Fi w portalu. Lokalna strona nadal jest chroniona hasłem administratora wybranym na urządzeniu.

## Rozwiązywanie problemów

### Bramka nie pojawia się w Home Assistant

- Sprawdź, czy Home Assistant ma dostęp do bramki w sieci IoT.
- Jeżeli wykrywanie między VLAN-ami jest filtrowane, dodaj integrację ESPHome ręcznie po adresie IP.
- Sprawdź, czy bramka jest online, i uruchom ponownie integrację ESPHome, jeżeli połączenie nadal jest niedostępne.

### Mesh jest gotowy, ale wartości pozostają niedostępne

- Umieść ESP32-C3 bliżej NightmatIQ i sprawdź **Ostatnie RSSI Mesh** w diagnostyce.
- Po imporcie kopii sieci poczekaj na synchronizację IV Index.
- Użyj przycisku **Odśwież**, aby zażądać aktualnego stanu.

### Nie udaje się pobrać sieci Steinel

- Sprawdź, czy konto ma dostęp do instalacji w oficjalnej aplikacji Steinel.
- Sprawdź dostęp do Internetu, DNS i czas systemowy w sieci bramki.
- Po nieudanym żądaniu konfiguracji poczekaj na restart bramki i spróbuj ponownie.

### Aktualizacja OTA nie działa

- Sprawdź adres urządzenia i hasło administratora bramki.
- Użyj aktualizacji z przeglądarki w zaufanej sieci LAN.
- Jeżeli urządzenie nie łączy się już z Wi-Fi, odzyskaj je przez natywny port USB.

## Powiązany projekt

Ta sama funkcjonalność Steinel NightmatIQ Plus jest również dostępna jako opcjonalna integracja w projekcie [AR01V3 RF/IR, ESP-RC01 & Steinel NightmatIQ Plus Gateway](https://github.com/supczinskib/athom-ar01v3-esp-rc01-gateway). Wybierz tamten projekt, jeśli NightmatIQ ma być dodatkiem do istniejącej wielofunkcyjnej bramki AR01V3; to repozytorium jest przeznaczone dla małej, dedykowanej instalacji ESP32-C3.

## Licencja

Copyright (C) 2026 Bartosz Supcziński.

Projekt jest udostępniany wyłącznie na warunkach GNU General Public License w wersji 3 (`GPL-3.0-only`). Pełna treść znajduje się w pliku [LICENSE](LICENSE).

## Autor i wsparcie

- Autor i opiekun projektu: **Bartosz Supcziński**, <bartek@env.pl>.
- Identyfikator projektu ESPHome: `envpl.steinel_nightmatiq_gateway`.

Zgłaszając problem, podaj wersję firmware, wersję ESPHome, przyczynę ostatniego restartu i odpowiednie logi. Przed udostępnieniem diagnostyki usuń hasła, klucze, nagłówki autoryzacji, prywatne kopie i identyfikatory sieci.

To niezależny projekt społecznościowy, który nie jest oficjalnym produktem Steinel, ESPHome ani Home Assistant.
