# IRCord Android Client — Arkkitehtuuri

**Stack:** Kotlin · Jetpack Compose · C++ (NDK) · Protobuf · Room · Hilt

---

## 1. Komponentit yhdellä silmäyksellä

```
┌───────────────────────────────────────────────────────────────────┐
│                     ircord-android                                 │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                    UI Layer (Compose)                        │ │
│  │   Screens · Components · Theme · Navigation                 │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                               │                                   │
│  ┌────────────────────────────┴────────────────────────────────┐ │
│  │              ViewModel Layer (Kotlin)                        │ │
│  │   ChatVM · VoiceVM · SettingsVM · ChannelListVM             │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                               │                                   │
│  ┌────────────────────────────┴────────────────────────────────┐ │
│  │            Repository / UseCase Layer (Kotlin)               │ │
│  │   MessageRepo · ChannelRepo · KeyRepo · VoiceRepo           │ │
│  └──────┬─────────────────┬─────────────────┬──────────────────┘ │
│         │                 │                 │                     │
│  ┌──────┴──────┐  ┌───────┴───────┐  ┌─────┴──────────────────┐ │
│  │  NetClient  │  │  LocalStore   │  │  Native Bridge (JNI)   │ │
│  │  (OkHttp /  │  │  (Room DB)    │  │  ┌──────────────────┐  │ │
│  │   raw TLS   │  │               │  │  │  CryptoEngine    │  │ │
│  │   socket)   │  │               │  │  │  (libsignal +    │  │ │
│  └─────────────┘  └───────────────┘  │  │   libsodium)     │  │ │
│                                       │  ├──────────────────┤  │ │
│                                       │  │  VoiceEngine     │  │ │
│                                       │  │  (libdatachannel │  │ │
│                                       │  │   + Opus + Oboe) │  │ │
│                                       │  └──────────────────┘  │ │
│                                       └────────────────────────┘ │
└───────────────────────────────────────────────────────────────────┘
```

### Miksi Kotlin + NDK (C++) hybridimalli?

IRCord-serveri ja desktop-client ovat C++20. Signal Protocol -sessiot, Opus-codec ja
libdatachannel ovat C/C++ -kirjastoja. Android-client jakaa **saman natiivikerroksen**
desktop-clientin kanssa — krypto- ja voice-koodi kirjoitetaan kerran ja JNI-bridgetään.

| Kerros | Kieli | Rationale |
|--------|-------|-----------|
| UI + ViewModel | Kotlin | Compose on paras Android UI toolkit, lifecycle-tuki |
| Repo / UseCase | Kotlin | Coroutines, Room, Hilt — Android-ekosysteemi |
| Network framing | Kotlin | OkHttp/raw socket + Protobuf-lite, Android-natiivi TLS |
| Crypto (Signal) | C++ (NDK) | Jaettu desktop-clientin kanssa, libsignal-protocol-c |
| Voice | C++ (NDK) | Jaettu desktop-clientin kanssa, libdatachannel + Opus |
| Audio I/O | C++ (NDK) | Oboe (Googlen low-latency Android audio API) |

---

## 2. Thread Model

```
┌─────────────────────────────────────────────────────────────┐
│  Main Thread (UI)                                            │
│   Compose recomposition, user input, navigation              │
│   → EI koskaan blokkaa — kaikki raskas työ coroutineissa    │
└────────────────────┬────────────────────────────────────────┘
                     │ StateFlow / SharedFlow
         ┌───────────┼───────────────┬──────────────────┐
         ▼           ▼               ▼                  ▼
┌──────────────┐ ┌──────────┐ ┌───────────────┐ ┌────────────┐
│  IO Dispatcher│ │ Default  │ │  NDK Thread   │ │ Audio      │
│  (Coroutine) │ │Dispatcher│ │  Pool         │ │ Thread     │
│              │ │          │ │  (C++ side)   │ │ (Oboe      │
│ - TLS socket │ │ - Proto  │ │              │ │  callback, │
│ - Room DB    │ │   parse  │ │ - Signal ops │ │  realtime) │
│ - Reconnect  │ │ - State  │ │ - ICE/DTLS   │ │            │
│              │ │   update │ │ - Opus enc/  │ │ - Capture  │
│              │ │          │ │   decode     │ │ - Playback │
└──────────────┘ └──────────┘ └───────────────┘ └────────────┘
```

**Kriittiset säännöt:**

1. **UI Thread ei koskaan blokkaa** — kaikki I/O ja krypto coroutineissa
2. **NDK-kutsut ovat kalliita** — JNI boundary overhead ~1-5µs per call, batchaa kun mahdollista
3. **Audio thread (Oboe callback)** on realtime — ei allokointeja, ei lockkeja, ei JNI-kutsuja
4. **Signal Protocol -operaatiot** voivat kestää 1-10ms (X3DH erityisesti) — aina Default dispatcherissa

---

## 3. JNI Bridge — Shared Native Layer

Desktop-clientin `crypto/` ja `voice/` -kansiot jaetaan suoraan. Android-spesifinen
lisäys on JNI-kerros ja Oboe (miniaudio:n sijaan).

```
ircord-native/                    ← Jaettu C++ moduuli
├── CMakeLists.txt
├── crypto/
│   ├── crypto_engine.hpp/.cpp    ← Sama kuin desktop
│   ├── signal_store.hpp/.cpp     ← Sama kuin desktop
│   └── ...
├── voice/
│   ├── voice_engine.hpp/.cpp     ← Sama kuin desktop
│   ├── opus_codec.hpp/.cpp       ← Sama kuin desktop
│   └── jitter_buffer.hpp/.cpp    ← Sama kuin desktop
├── audio/
│   ├── oboe_device.hpp/.cpp      ← Android-spesifinen (korvaa miniaudio)
│   └── audio_device.hpp          ← Abstrakti interface (yhteinen)
└── jni/
    ├── crypto_jni.cpp            ← JNI bridget CryptoEngineen
    ├── voice_jni.cpp             ← JNI bridget VoiceEngineen
    └── jni_helpers.hpp           ← Utility: jstring↔std::string, exceptions
```

### JNI Interface (Kotlin-puoli)

```kotlin
object NativeCrypto {
    init { System.loadLibrary("ircord-native") }

    // Identity
    external fun generateIdentity(): ByteArray          // → identity pub
    external fun prepareRegistration(numOpks: Int): ByteArray // → KeyUpload proto

    // Encrypt/decrypt
    external fun encrypt(recipientId: String, plaintext: ByteArray): ByteArray
    external fun decrypt(senderId: String, ciphertext: ByteArray, type: Int): ByteArray

    // Group (Sender Keys)
    external fun initGroupSession(channelId: String, members: Array<String>)
    external fun encryptGroup(channelId: String, plaintext: ByteArray): ByteArray
    external fun decryptGroup(senderId: String, channelId: String, ciphertext: ByteArray): ByteArray

    // Safety Number
    external fun safetyNumber(peerId: String): String

    // Auth
    external fun signChallenge(nonce: ByteArray): ByteArray
}

object NativeVoice {
    init { System.loadLibrary("ircord-native") }

    external fun init(sampleRate: Int, framesPerBuffer: Int)
    external fun joinRoom(channelId: String)
    external fun leaveRoom()
    external fun call(peerId: String)
    external fun hangup()
    external fun setMuted(muted: Boolean)
    external fun setDeafened(deafened: Boolean)
    external fun onVoiceSignal(fromUser: String, signalType: Int, data: ByteArray)
    external fun destroy()

    // Callback: voice engine kutsuu näitä C++:sta → Kotlin
    // Rekisteröidään init():ssä
    interface VoiceCallback {
        fun onIceCandidate(peerId: String, candidate: ByteArray)
        fun onPeerJoined(peerId: String)
        fun onPeerLeft(peerId: String)
        fun onAudioLevel(peerId: String, level: Float)
    }
}
```

---

## 4. Hakemistorakenne

```
ircord-android/
├── app/
│   ├── build.gradle.kts
│   └── src/main/
│       ├── java/fi/ircord/android/
│       │   ├── IrcordApp.kt                  # Application, Hilt entry
│       │   ├── MainActivity.kt               # Single Activity
│       │   │
│       │   ├── di/                            # Hilt modules
│       │   │   ├── AppModule.kt
│       │   │   ├── NetworkModule.kt
│       │   │   └── DatabaseModule.kt
│       │   │
│       │   ├── data/
│       │   │   ├── local/
│       │   │   │   ├── IrcordDatabase.kt      # Room DB
│       │   │   │   ├── dao/
│       │   │   │   │   ├── MessageDao.kt
│       │   │   │   │   ├── ChannelDao.kt
│       │   │   │   │   └── PeerIdentityDao.kt
│       │   │   │   └── entity/
│       │   │   │       ├── MessageEntity.kt
│       │   │   │       ├── ChannelEntity.kt
│       │   │   │       └── PeerIdentityEntity.kt
│       │   │   ├── remote/
│       │   │   │   ├── IrcordSocket.kt        # TLS TCP framing
│       │   │   │   ├── FrameCodec.kt          # Length-prefixed read/write
│       │   │   │   └── ReconnectPolicy.kt     # Exp backoff
│       │   │   └── repository/
│       │   │       ├── MessageRepository.kt
│       │   │       ├── ChannelRepository.kt
│       │   │       ├── KeyRepository.kt
│       │   │       └── VoiceRepository.kt
│       │   │
│       │   ├── domain/
│       │   │   ├── model/
│       │   │   │   ├── Message.kt
│       │   │   │   ├── Channel.kt
│       │   │   │   ├── User.kt
│       │   │   │   └── VoiceState.kt
│       │   │   └── usecase/
│       │   │       ├── SendMessageUseCase.kt
│       │   │       ├── JoinChannelUseCase.kt
│       │   │       └── StartCallUseCase.kt
│       │   │
│       │   ├── ui/
│       │   │   ├── theme/
│       │   │   │   ├── Theme.kt               # Material3 + IrcordTheme
│       │   │   │   ├── Color.kt               # Design tokens
│       │   │   │   ├── Type.kt                # Typography
│       │   │   │   └── Spacing.kt             # Dimension tokens
│       │   │   ├── navigation/
│       │   │   │   └── IrcordNavGraph.kt
│       │   │   ├── screen/
│       │   │   │   ├── chat/
│       │   │   │   │   ├── ChatScreen.kt
│       │   │   │   │   ├── ChatViewModel.kt
│       │   │   │   │   └── components/
│       │   │   │   │       ├── MessageBubble.kt
│       │   │   │   │       ├── LinkPreviewCard.kt
│       │   │   │   │       ├── MessageInput.kt
│       │   │   │   │       └── TypingIndicator.kt
│       │   │   │   ├── channels/
│       │   │   │   │   ├── ChannelListScreen.kt
│       │   │   │   │   └── ChannelListViewModel.kt
│       │   │   │   ├── voice/
│       │   │   │   │   ├── VoiceOverlay.kt
│       │   │   │   │   ├── CallScreen.kt
│       │   │   │   │   └── VoiceViewModel.kt
│       │   │   │   ├── settings/
│       │   │   │   │   ├── SettingsScreen.kt
│       │   │   │   │   └── SettingsViewModel.kt
│       │   │   │   ├── auth/
│       │   │   │   │   ├── SetupScreen.kt     # First-run: generate keys
│       │   │   │   │   └── SetupViewModel.kt
│       │   │   │   └── verify/
│       │   │   │       └── SafetyNumberScreen.kt
│       │   │   └── components/                # Shared composables
│       │   │       ├── UserAvatar.kt
│       │   │       ├── StatusBadge.kt
│       │   │       ├── EncryptionBadge.kt
│       │   │       └── VoicePill.kt
│       │   │
│       │   ├── native/                        # JNI bridge (Kotlin side)
│       │   │   ├── NativeCrypto.kt
│       │   │   └── NativeVoice.kt
│       │   │
│       │   └── service/
│       │       ├── IrcordService.kt           # Foreground Service (connection)
│       │       └── VoiceService.kt            # Foreground Service (active call)
│       │
│       ├── res/
│       │   ├── values/
│       │   │   ├── strings.xml
│       │   │   └── themes.xml
│       │   └── drawable/
│       │
│       └── cpp/                               # NDK source (tai erillinen moduuli)
│           └── → symlink/copy ircord-native/
│
├── native/                                    # Shared C++ moduuli
│   ├── CMakeLists.txt
│   └── → ircord-native/ (ks. yllä)
│
├── build.gradle.kts                           # Project-level
├── settings.gradle.kts
└── gradle.properties
```

---

## 5. Foreground Services

Android vaatii Foreground Servicen pitkäaikaisille yhteyksille ja äänelle.

### IrcordService — Persistent Connection

```kotlin
class IrcordService : Service() {
    // Pitää TCP/TLS-yhteyden serveriin hengissä
    // Näyttää notificationin: "IrssiCord — Connected"
    // Käsittelee reconnectin, offline-viestien vastaanoton
    // Lifecycle: käynnistyy kun app avataan, sammuu kun user kirjautuu ulos

    // Doze mode: käyttää WorkManager periodic taskia ping-keepaliveen
    // Tai: Firebase Cloud Messaging fallback offline-notifikaatioihin
}
```

### VoiceService — Active Call

```kotlin
class VoiceService : Service() {
    // Aktiivinen kun voice room tai puhelu on käynnissä
    // Pitää audio-streamin hengissä
    // Notification: "Voice — #general (3 users)" tai "Call — Matti"
    // Wakelock: PARTIAL_WAKE_LOCK äänelle
    // Audio focus: requestAudioFocus(USAGE_VOICE_COMMUNICATION)
}
```

---

## 6. Dataflow: Viestin lähetys

```
User painaa Send
  → ChatViewModel.sendMessage(text)
    → SendMessageUseCase.invoke(channelId, text)
      → NativeCrypto.encrypt(recipientId, text.toByteArray())
        └── [JNI → C++ CryptoEngine::encrypt() → Signal Protocol]
      → Envelope(type=CHAT, payload=ciphertext)
      → IrcordSocket.send(envelope)
        └── [length-prefix + protobuf serialize → TLS write]
      → MessageRepository.insertLocal(msg, status=SENDING)
        └── [Room DAO → SQLite]
      → UI: MessageBubble shows ⏳

Server relay → recipient
  → IrcordSocket.receive() [suspend, IO dispatcher]
    → FrameCodec.decode() → Envelope
    → MessageHandler.dispatch(envelope)
      → NativeCrypto.decrypt(senderId, ciphertext, type)
        └── [JNI → C++ CryptoEngine::decrypt()]
      → MessageRepository.insert(decryptedMsg)
      → NotificationManager (jos app ei ole foreground)
      → ChatViewModel.messages StateFlow päivittyy
        → Compose recomposition → uusi viesti näkyy
```

---

## 7. Offline & Background -strategia

### Tasot (priority-järjestys)

| Taso | Mekanismi | Latenssi | Battery |
|------|-----------|----------|---------|
| 1. Foreground | IrcordService, TCP keepalive | <100ms | Korkea |
| 2. Background | WorkManager periodic (15min) | ~15min | Matala |
| 3. Push | FCM data message → wake service | ~1-5s | Matala |

**Suositus:** Foreground service kun app on näkyvissä tai recent-listalla.
FCM fallback kun app on täysin suljettu. Serveri lähettää FCM push-notificationin
(ei sisällä viestin tekstiä — E2E!) kun offline-viestejä kertyy.

### FCM Integration (E2E-ystävällinen)

```
Serveri havaitsee: user offline, offline_queue kasvaa
  → Serveri lähettää FCM:lle: { "type": "wakeup", "count": 3 }
  → Android vastaanottaa: käynnistä IrcordService
  → IrcordService yhdistää serveriin
  → Serveri lähettää offline-viestit
  → CryptoEngine purkaa E2E
  → NotificationManager: "3 uutta viestiä"
```

**Tärkeä:** FCM payload ei sisällä viestin sisältöä — serveri ei tiedä sitä.

---

## 8. Local Storage — Room Schema

Room-entiteetit vastaavat desktop-clientin SQLite-schemaa:

```kotlin
@Entity(tableName = "messages",
        indices = [Index("channel_id", "timestamp")])
data class MessageEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    @ColumnInfo(name = "channel_id") val channelId: String,
    @ColumnInfo(name = "sender_id") val senderId: String,
    val content: String,
    val timestamp: Long,
    @ColumnInfo(name = "msg_type") val msgType: String = "chat",
    @ColumnInfo(name = "send_status") val sendStatus: String = "sent"
    // "sending" | "sent" | "failed"
)

@Entity(tableName = "channels")
data class ChannelEntity(
    @PrimaryKey @ColumnInfo(name = "channel_id") val channelId: String,
    @ColumnInfo(name = "display_name") val displayName: String?,
    @ColumnInfo(name = "joined_at") val joinedAt: Long,
    @ColumnInfo(name = "last_read_ts") val lastReadTs: Long = 0
)

@Entity(tableName = "peer_identities")
data class PeerIdentityEntity(
    @PrimaryKey @ColumnInfo(name = "user_id") val userId: String,
    @ColumnInfo(name = "identity_pub") val identityPub: ByteArray,
    @ColumnInfo(name = "trust_status") val trustStatus: String = "unverified",
    @ColumnInfo(name = "safety_number") val safetyNumber: String?
)
```

**Crypto-avaimet** (identity key, Signal sessions jne.) tallennetaan
**Android Keystore** + **EncryptedSharedPreferences** tai erilliseen
salattuun SQLite-tietokantaan (SQLCipher).

---

## 9. Turvallisuus — Android-spesifiset huomiot

| Uhka | Suojaus |
|------|---------|
| Root/jailbreak | Detect & warn, mutta älä estä (kaveriporukka) |
| Screen capture | FLAG_SECURE chat-näkymässä (konfiguroitava) |
| Key extraction | Android Keystore hardware-backed (TEE/StrongBox) |
| Network intercept | Certificate pinning (OkHttp CertificatePinner) |
| Backup leak | `android:allowBackup="false"`, encrypted Room DB |
| Clipboard snoop | Clear clipboard 30s after Safety Number copy |
| Push content leak | FCM: vain "wakeup" — ei viestin sisältöä |

---

## 10. Build & NDK Setup

```kotlin
// app/build.gradle.kts
android {
    ndkVersion = "27.0.12077973"

    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += "-DANDROID_STL=c++_shared"
                // Tuetut ABI:t
                abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
```

### NDK CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.22)
project(ircord-native)

set(CMAKE_CXX_STANDARD 20)

# Shared C++ koodi (symlinkattu desktop-clientistä)
add_library(ircord-native SHARED
    jni/crypto_jni.cpp
    jni/voice_jni.cpp
    crypto/crypto_engine.cpp
    crypto/signal_store.cpp
    voice/voice_engine.cpp
    voice/opus_codec.cpp
    voice/jitter_buffer.cpp
    audio/oboe_device.cpp
)

# Pre-built NDK-kirjastot
find_package(oboe REQUIRED CONFIG)
target_link_libraries(ircord-native
    oboe::oboe
    # Nämä joko prefab-pakettina tai vcpkg android tripletillä:
    libsignal-protocol-c
    sodium
    libdatachannel
    opus
    log  # Android log
)
```
