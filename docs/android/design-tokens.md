# IRCord Android — Design Tokens

Yhtenäinen visuaalinen kieli koko Android-clientille. Perustuu desktop-clientin
Tokyo Night -teemaan, adaptoituna Material 3 -järjestelmään.

---

## 1. Väripaletti

### Perusvärit (Tokyo Night → Material 3 mapping)

Desktop-clientin `ColorScheme` adaptoituna Androidin dark/light -teemaksi.
**Primary mode: Dark** — IrssiCord on terminaali-henkinen, dark-first.

```
┌──────────────────────────────────────────────────────────────────┐
│  DARK THEME (default)                                            │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Background                                                      │
│  ┌────────┐ ┌────────┐ ┌────────┐                               │
│  │ #1a1b26│ │ #16161e│ │ #24283b│                               │
│  │ bg     │ │ bg-deep│ │ surface│                               │
│  └────────┘ └────────┘ └────────┘                               │
│                                                                  │
│  Text                                                            │
│  ┌────────┐ ┌────────┐ ┌────────┐                               │
│  │ #c0caf5│ │ #a9b1d6│ │ #565f89│                               │
│  │ primary│ │ second.│ │ muted  │                               │
│  └────────┘ └────────┘ └────────┘                               │
│                                                                  │
│  Accent                                                          │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐                   │
│  │ #7aa2f7│ │ #9ece6a│ │ #e0af68│ │ #f7768e│                   │
│  │ blue   │ │ green  │ │ amber  │ │ red    │                   │
│  └────────┘ └────────┘ └────────┘ └────────┘                   │
│                                                                  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐                   │
│  │ #bb9af7│ │ #7dcfff│ │ #73daca│ │ #ff9e64│                   │
│  │ purple │ │ cyan   │ │ teal   │ │ orange │                   │
│  └────────┘ └────────┘ └────────┘ └────────┘                   │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Material 3 Token Mapping

```kotlin
// Color.kt

// ── Dark Theme ──────────────────────────────────
val DarkColorScheme = darkColorScheme(
    // Primary
    primary          = Color(0xFF7AA2F7),  // tokyo-night blue
    onPrimary        = Color(0xFF1A1B26),
    primaryContainer = Color(0xFF2A3A6B),
    onPrimaryContainer = Color(0xFFBBD6FF),

    // Secondary
    secondary          = Color(0xFF9ECE6A),  // tokyo-night green
    onSecondary        = Color(0xFF1A1B26),
    secondaryContainer = Color(0xFF2D4A1E),
    onSecondaryContainer = Color(0xFFD4F5A0),

    // Tertiary
    tertiary          = Color(0xFFBB9AF7),  // tokyo-night purple
    onTertiary        = Color(0xFF1A1B26),
    tertiaryContainer = Color(0xFF3D2D5C),
    onTertiaryContainer = Color(0xFFE3D0FF),

    // Error
    error          = Color(0xFFF7768E),     // tokyo-night red
    onError        = Color(0xFF1A1B26),
    errorContainer = Color(0xFF5C1D28),
    onErrorContainer = Color(0xFFFFB3C0),

    // Background & Surface
    background     = Color(0xFF1A1B26),     // tokyo-night bg
    onBackground   = Color(0xFFC0CAF5),
    surface        = Color(0xFF24283B),     // tokyo-night surface
    onSurface      = Color(0xFFC0CAF5),
    surfaceVariant = Color(0xFF2F3549),
    onSurfaceVariant = Color(0xFFA9B1D6),

    // Outline
    outline        = Color(0xFF3B4261),
    outlineVariant = Color(0xFF2F3549),

    // Inverse
    inverseSurface   = Color(0xFFC0CAF5),
    inverseOnSurface = Color(0xFF1A1B26),
    inversePrimary   = Color(0xFF3D5CC0),
)

// ── Light Theme (secondary, vaihtoehtona) ───────
val LightColorScheme = lightColorScheme(
    primary          = Color(0xFF3D5CC0),
    onPrimary        = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFDBE1FF),
    onPrimaryContainer = Color(0xFF001849),

    secondary          = Color(0xFF4D7A2A),
    onSecondary        = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFCEF4A0),
    onSecondaryContainer = Color(0xFF0F2000),

    background     = Color(0xFFF5F5F8),
    onBackground   = Color(0xFF1A1B26),
    surface        = Color(0xFFFFFFFF),
    onSurface      = Color(0xFF1A1B26),

    error          = Color(0xFFBA1A1A),
    outline        = Color(0xFF757889),
)
```

### Semanttiset värit (IrssiCord-spesifiset)

```kotlin
// IrcordColors.kt — M3:n päälle rakennettavat semanttiset värit

data class IrcordSemanticColors(
    // Timestamps
    val timestamp: Color,

    // Nick-värit (hash-pohjaiset, kuten desktop)
    val nickPalette: List<Color>,

    // System messages
    val systemMessage: Color,

    // Encryption status
    val encryptionOk: Color,         // 🔒 E2E verified
    val encryptionUnverified: Color, // ⚠️ not yet verified
    val encryptionWarning: Color,    // ❌ key changed

    // Voice
    val voiceActive: Color,
    val voiceMuted: Color,
    val voiceSpeaking: Color,        // glow/ring color

    // Unread
    val unreadBadge: Color,
    val mentionBadge: Color,

    // Link preview
    val previewBorder: Color,
    val previewTitle: Color,

    // Online status
    val statusOnline: Color,
    val statusAway: Color,
    val statusOffline: Color,

    // Input
    val inputBackground: Color,
    val inputPlaceholder: Color,
)

val DarkIrcordColors = IrcordSemanticColors(
    timestamp          = Color(0xFF565F89),
    nickPalette        = listOf(
        Color(0xFF7AA2F7),  // blue
        Color(0xFF9ECE6A),  // green
        Color(0xFFE0AF68),  // amber
        Color(0xFFBB9AF7),  // purple
        Color(0xFF7DCFFF),  // cyan
        Color(0xFFF7768E),  // red
        Color(0xFF73DACA),  // teal
        Color(0xFFFF9E64),  // orange
    ),
    systemMessage      = Color(0xFFE0AF68),
    encryptionOk       = Color(0xFF9ECE6A),
    encryptionUnverified = Color(0xFFE0AF68),
    encryptionWarning  = Color(0xFFF7768E),
    voiceActive        = Color(0xFF9ECE6A),
    voiceMuted         = Color(0xFFF7768E),
    voiceSpeaking      = Color(0xFF73DACA),
    unreadBadge        = Color(0xFFF7768E),
    mentionBadge       = Color(0xFFFF9E64),
    previewBorder      = Color(0xFF3B4261),
    previewTitle       = Color(0xFF7AA2F7),
    statusOnline       = Color(0xFF9ECE6A),
    statusAway         = Color(0xFFE0AF68),
    statusOffline      = Color(0xFF565F89),
    inputBackground    = Color(0xFF16161E),
    inputPlaceholder   = Color(0xFF565F89),
)
```

### Nick Color -funktio (sama logiikka kuin desktop)

```kotlin
fun nickColor(nick: String, palette: List<Color>): Color {
    val hash = nick.hashCode().toUInt()
    return palette[(hash % palette.size.toUInt()).toInt()]
}
```

---

## 2. Typography

Monospace-henkinen typografia terminaali-estetiikan mukaisesti.

```kotlin
// Type.kt

val IrcordTypography = Typography(
    // ── Display (ei käytössä, mutta M3 vaatii) ──
    displayLarge  = default,
    displayMedium = default,
    displaySmall  = default,

    // ── Headline ────────────────────────────────
    headlineLarge  = TextStyle(
        fontFamily = FontFamily.Default,  // tai JetBrains Mono
        fontWeight = FontWeight.Bold,
        fontSize   = 24.sp,
        lineHeight = 32.sp,
    ),
    headlineMedium = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.SemiBold,
        fontSize   = 20.sp,
        lineHeight = 28.sp,
    ),

    // ── Title (channel names, section headers) ──
    titleLarge = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.SemiBold,
        fontSize   = 18.sp,
        lineHeight = 24.sp,
    ),
    titleMedium = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Medium,
        fontSize   = 16.sp,
        lineHeight = 22.sp,
    ),
    titleSmall = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Medium,
        fontSize   = 14.sp,
        lineHeight = 20.sp,
    ),

    // ── Body (messages, descriptions) ───────────
    bodyLarge = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Normal,
        fontSize   = 16.sp,
        lineHeight = 24.sp,
        letterSpacing = 0.15.sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Normal,
        fontSize   = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.25.sp,
    ),
    bodySmall = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Normal,
        fontSize   = 12.sp,
        lineHeight = 16.sp,
        letterSpacing = 0.4.sp,
    ),

    // ── Label (buttons, badges, timestamps) ─────
    labelLarge = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Medium,
        fontSize   = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.1.sp,
    ),
    labelMedium = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Medium,
        fontSize   = 12.sp,
        lineHeight = 16.sp,
        letterSpacing = 0.5.sp,
    ),
    labelSmall = TextStyle(
        fontFamily = FontFamily.Default,
        fontWeight = FontWeight.Medium,
        fontSize   = 10.sp,
        lineHeight = 14.sp,
        letterSpacing = 0.5.sp,
    ),
)
```

### Monospace (koodiblokille ja Safety Numberille)

```kotlin
val MonoFamily = FontFamily(
    Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
    Font(R.font.jetbrains_mono_bold, FontWeight.Bold),
)

// Fallback jos fontti ei bundleta:
val MonoFamily = FontFamily.Monospace

val MonoStyle = TextStyle(
    fontFamily = MonoFamily,
    fontSize   = 14.sp,
    lineHeight = 20.sp,
    letterSpacing = 0.sp,
)
```

### Käyttökohteet

| Tyyli | Käyttö |
|-------|--------|
| `titleMedium` | Channel nimi sidebarissa |
| `titleSmall` | Nick viestissä |
| `bodyLarge` | Viestin teksti |
| `bodySmall` | Link preview description |
| `labelSmall` | Timestamp, badge count |
| `MonoStyle` | Safety Number, koodi-viestit |

---

## 3. Spacing & Dimensions

```kotlin
// Spacing.kt

object IrcordSpacing {
    // ── Base grid: 4dp ──────────────────────────
    val xxs = 2.dp
    val xs  = 4.dp
    val sm  = 8.dp
    val md  = 12.dp
    val lg  = 16.dp
    val xl  = 24.dp
    val xxl = 32.dp
    val xxxl = 48.dp

    // ── Component-specific ──────────────────────

    // Message list
    val messagePaddingHorizontal = 16.dp
    val messagePaddingVertical   = 4.dp     // Tiivis, IRC-tyylinen
    val messageGapBetween        = 2.dp     // Minimaali gap viestien välillä
    val messageGapSameSender     = 1.dp     // Peräkkäiset saman lähettäjän viestit
    val messageNickWidth         = 100.dp   // Max nick-leveys (truncate)

    // Channel sidebar / drawer
    val drawerWidth              = 280.dp
    val channelItemHeight        = 48.dp
    val channelItemPadding       = 12.dp
    val channelIconSize          = 20.dp

    // Input bar
    val inputBarHeight           = 56.dp
    val inputBarPadding          = 8.dp
    val inputCornerRadius        = 24.dp

    // Link preview card
    val previewCardPadding       = 12.dp
    val previewCardMarginStart   = 48.dp   // Indent viestin alle
    val previewCardCornerRadius  = 8.dp
    val previewCardMaxWidth      = 320.dp

    // Voice overlay
    val voicePillHeight          = 40.dp
    val voicePillCornerRadius    = 20.dp
    val voiceAvatarSize          = 32.dp

    // Status indicators
    val statusDotSize            = 8.dp
    val unreadBadgeSize          = 18.dp
    val unreadBadgeCornerRadius  = 9.dp

    // Top bar
    val topBarHeight             = 56.dp

    // Bottom bar (voice controls, jos aktiivinen)
    val bottomBarHeight          = 64.dp

    // Safety Number grid
    val safetyNumberCellWidth    = 56.dp
    val safetyNumberCellHeight   = 32.dp
    val safetyNumberGap          = 8.dp
}
```

---

## 4. Elevation & Shadows

```kotlin
// Elevation.kt — Material 3 tone-based elevation

object IrcordElevation {
    val level0 = 0.dp     // Background
    val level1 = 1.dp     // Surface, channel list items
    val level2 = 3.dp     // Cards, link preview
    val level3 = 6.dp     // Modal bottom sheet, drawer
    val level4 = 8.dp     // Dialog
    val level5 = 12.dp    // FAB (ei käytössä)
}
```

Material 3 dark themessä elevation muuttaa surface-värin sävyä (tonal elevation)
eikä lisää varjoja — tämä sopii hyvin terminal-estetiikkaan.

---

## 5. Shape / Corner Radius

```kotlin
// Shape.kt

val IrcordShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),    // Badges, chips
    small      = RoundedCornerShape(8.dp),    // Cards, preview
    medium     = RoundedCornerShape(12.dp),   // Dialogs
    large      = RoundedCornerShape(16.dp),   // Bottom sheets
    extraLarge = RoundedCornerShape(24.dp),   // Input field
)
```

### Viesti-kupla vs IRC-tyyli

IrssiCord suosii **flat IRC-tyyliä** (ei kuplia) oletuksena:

```
                    IRC mode (default)           Bubble mode (vaihtoehto)
                    ──────────────────           ────────────────────────
                    [21:33] Matti: moi           ┌──────────────┐
                    [21:34] Teppo: yo            │ moi          │ Matti
                    [21:34] Sepi: jep            └──────────────┘
                                                        ┌──────┐
                                                  Teppo │ yo   │
                                                        └──────┘
```

Asetus: `Settings > Appearance > Message style: IRC / Bubble`

---

## 6. Iconography

Material Symbols (Outlined, weight 400) + muutama custom:

| Ikoni | Symbol | Käyttö |
|-------|--------|--------|
| Channel (text) | `tag` | #kanava-listassa |
| Channel (voice) | `mic` | Voice room -kanava |
| Lock | `lock` | E2E-salaus OK |
| Lock open | `lock_open` | Salaus unverified |
| Warning | `warning` | Key changed |
| Send | `send` | Viestin lähetys |
| Mute | `mic_off` | Mikrofoni muted |
| Deafen | `headset_off` | Ääni muted |
| Call | `call` | Private call |
| Hangup | `call_end` | Lopeta puhelu |
| Settings | `settings` | Asetukset |
| Person | `person` | User avatar fallback |
| Group | `group` | Voice participants |
| Link | `link` | Link preview |
| Search | `search` | Viestihaku |
| Verified | `verified_user` | Safety Number OK |

---

## 7. Animation Tokens

```kotlin
object IrcordMotion {
    // Durations
    val durationFast   = 150    // ms — badge appear, status dot
    val durationMedium = 300    // ms — drawer open, card expand
    val durationSlow   = 500    // ms — screen transition

    // Easing
    val easingStandard  = CubicBezierEasing(0.2f, 0.0f, 0.0f, 1.0f)  // M3 standard
    val easingDecelerate = CubicBezierEasing(0.0f, 0.0f, 0.0f, 1.0f)
    val easingAccelerate = CubicBezierEasing(0.3f, 0.0f, 1.0f, 1.0f)

    // Voice-spesifiset
    val speakingPulseInterval = 100  // ms — speaking indicator pulse
    val speakingGlowRadius    = 4.dp
}
```

---

## 8. Dark/Light Mode -strategia

| Aspekti | Dark (default) | Light |
|---------|---------------|-------|
| Status bar | Transparent, light icons | Transparent, dark icons |
| Nav bar | `#16161e` | `#f5f5f8` |
| Splash | `#1a1b26` + logo | `#f5f5f8` + logo |
| Notifikaatiot | System default | System default |

**Suositus:** Dark mode on primary. Light mode tuetaan mutta ei priorisoida —
käyttäjäkunta on terminaalikäyttäjiä jotka suosivat tummaa.

App seuraa Android system settingiä (`isSystemInDarkTheme()`) ellei
käyttäjä valitse override-asetusta: `Dark / Light / System`.
