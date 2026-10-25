package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CutCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Text
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.R
import kotlinx.coroutines.delay

// Design system for the OpenTrailPaper companion app — a port of
// companion-ios/Sources/Theme.swift, so the two phones look like one product.
//
// The app looks like the e-paper head unit it pairs with: pure white paper,
// pure black ink, square corners, 2dp black rules around tiles and cards, and
// heavy condensed numerals. Emphasis is an INVERTED tile (black, white type) —
// the selected tab, primary buttons, done/connected/uploaded — rather than a
// colour. Warnings are a hatched panel, the way the device shades an area.
// The only colour left is a service's own logo (Strava's orange tile, the
// intervals.icu and Ride with GPS marks).

object Palette {
    val paper = Color.White                // app background
    val surface = Color.White              // cards
    val ink = Color.Black                  // primary text, rules
    /** Secondary text. Near-black: hierarchy comes from size and weight, not
     *  from fading the ink (16:1 on white). */
    val muted = Color(0xFF262626)
    /** Placeholders and disabled controls only. 4.5:1 on white (WCAG AA). */
    val faint = Color(0xFF767676)
    /** Borders. Black like everything else; the weight is in [Rule]. */
    val hairline = Color.Black
    /** Actions and live data — the ink itself now; emphasis is inversion. */
    val accent = Color.Black
    val accentDark = Color.Black
    /** Behind a selected chip in screens not rebuilt with inverted tiles. */
    val accentWash = Color(0xFFEDEDED)
    val accentInk = Color.White
    /** "Connected / success". Ink: done states are inverted, not green. */
    val good = Color.Black
    /** Map hexes due an update — a mid grey, so two states still differ on
     *  the same map without a badge. */
    val update = Color(0xFF767676)
    /** Strava's brand orange, for its logo tile only. */
    val strava = Color(0xFFFC4C02)
}

/** Rule weights: tiles and cards, the line under a screen title, dividers. */
object Rule {
    val border = 2.dp
    val title = 3.dp
    val divider = 2.dp
}

// Bundled faces. Anton (heavy condensed) for numerals and screen titles,
// Barlow Condensed for compact labels, Barlow for body copy.
val BarlowCondensed = FontFamily(
    Font(R.font.barlowcondensedmedium, FontWeight.Medium),
    Font(R.font.barlowcondensedsemibold, FontWeight.SemiBold),
    Font(R.font.barlowcondensedbold, FontWeight.Bold),
)

val Barlow = FontFamily(
    Font(R.font.barlowregular, FontWeight.Normal),
    Font(R.font.barlowmedium, FontWeight.Medium),
    Font(R.font.barlowsemibold, FontWeight.SemiBold),
    Font(R.font.barlowbold, FontWeight.Bold),
)

/** Anton: the device's heavy condensed numerals and the screen titles. */
val Anton = FontFamily(Font(R.font.anton, FontWeight.Normal))

fun condensed(size: TextUnit, weight: FontWeight = FontWeight.SemiBold) =
    TextStyle(fontFamily = BarlowCondensed, fontWeight = weight, fontSize = size)

fun barlow(size: TextUnit, weight: FontWeight = FontWeight.Normal) =
    TextStyle(fontFamily = Barlow, fontWeight = weight, fontSize = size)

fun anton(size: TextUnit) = TextStyle(fontFamily = Anton, fontWeight = FontWeight.Normal, fontSize = size)

/** Tracked bold caps, the style of every small label and button. */
fun caps(size: TextUnit = 12.sp, tracking: TextUnit = 2.sp) =
    barlow(size, FontWeight.Bold).copy(letterSpacing = tracking)

object TypeScale {
    // Big numerals — Anton, echoing the device hero numbers.
    fun hero(size: TextUnit = 68.sp) = anton(size)
    fun value(size: TextUnit = 30.sp) = anton(size)
    val screenTitle = anton(40.sp).copy(letterSpacing = 1.sp)   // "RIDE" / "RIDES"
    val title = condensed(24.sp, FontWeight.Bold)
    val body = barlow(16.sp)
    val bodyStrong = barlow(15.sp, FontWeight.Bold)
    val label = barlow(12.sp, FontWeight.Bold)
}

/** Tracked-out bold caps label, matching the device's field captions. */
@Composable
fun TrackedLabel(text: String, modifier: Modifier = Modifier, color: Color = Palette.ink) {
    Text(
        text = text.uppercase(),
        modifier = modifier,
        style = TypeScale.label.copy(letterSpacing = 2.sp),
        color = color,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

// MARK: - Surfaces

/** A 2dp black square border, the shape of every tile. */
fun Modifier.einkBorder(width: Dp = Rule.border, color: Color = Palette.ink): Modifier =
    border(width, color)

/**
 * Diagonal lines behind the content, the device's way of shading an area —
 * used for warnings instead of a tint.
 */
fun Modifier.hatched(
    spacing: Dp = 6.dp,
    lineWidth: Dp = 1.2.dp,
    color: Color = Palette.ink.copy(alpha = 0.35f),
    background: Color = Palette.paper,
): Modifier = clipToBounds().drawBehind {
    drawRect(background)
    val step = spacing.toPx()
    val w = lineWidth.toPx()
    var x = -size.height
    while (x < size.width) {
        drawLine(color, Offset(x, size.height), Offset(x + size.height, 0f), strokeWidth = w)
        x += step
    }
}

/** A content card: white, square, a 2dp black border. */
@Composable
fun Card(
    modifier: Modifier = Modifier,
    padding: Dp = 16.dp,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Palette.surface)
            .einkBorder()
            .padding(padding),
        content = content,
    )
}

/**
 * A screen's big CAPS title with the 3dp rule under it, room for actions on
 * the right, and an optional full-width status line under the title row
 * (mockups: RIDE / RIDES / ROUTE / SETTINGS).
 */
@Composable
fun ScreenHeader(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: (@Composable () -> Unit)? = null,
    trailing: @Composable RowScope.() -> Unit = {},
) {
    Column(modifier.fillMaxWidth().background(Palette.paper)) {
        Column(
            Modifier.padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    title.uppercase(),
                    style = TypeScale.screenTitle,
                    color = Palette.ink,
                    maxLines = 1,
                    modifier = Modifier.weight(1f, fill = false).semantics { heading() },
                )
                Spacer(Modifier.weight(1f))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.padding(bottom = 4.dp),
                    content = trailing,
                )
            }
            subtitle?.invoke()
        }
        Box(Modifier.fillMaxWidth().height(Rule.title).background(Palette.ink))
    }
}

// MARK: - Grouped lists (Settings)

/** A section's tracked CAPS header. */
@Composable
fun SettingsSectionHeader(title: String, modifier: Modifier = Modifier) {
    TrackedLabel(title, modifier.fillMaxWidth().semantics { heading() })
}

/** A bordered group of rows; put a [GroupedDivider] between rows. */
@Composable
fun GroupedRows(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.fillMaxWidth().background(Palette.surface).einkBorder(),
        content = content,
    )
}

/** The 2dp rule between rows of a [GroupedRows]. */
@Composable
fun GroupedDivider() {
    Box(Modifier.fillMaxWidth().height(Rule.divider).background(Palette.ink))
}

// MARK: - Warnings

/**
 * A warning: bordered, hatched, with the text on a white plate so it stays
 * black on white for reading.
 */
@Composable
fun WarningBanner(
    title: String,
    modifier: Modifier = Modifier,
    message: String? = null,
    icon: ImageVector = Icons.Filled.Warning,
    accessory: (@Composable () -> Unit)? = null,
) {
    Box(
        modifier
            .fillMaxWidth()
            .hatched()
            .einkBorder()
            .padding(6.dp),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .background(Palette.paper)
                .padding(horizontal = 10.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Icon(icon, contentDescription = null, tint = Palette.ink, modifier = Modifier.size(18.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title.uppercase(), style = caps(12.sp, 1.5.sp), color = Palette.ink)
                if (message != null) {
                    Text(message, style = barlow(13.sp, FontWeight.Medium), color = Palette.ink)
                }
            }
            accessory?.invoke()
        }
    }
}

// MARK: - Chips and tags

/** A small CAPS tag: inverted (done / connected) or outlined (a state). */
@Composable
fun Tag(text: String, modifier: Modifier = Modifier, inverted: Boolean = false) {
    Text(
        text.uppercase(),
        style = caps(11.sp, 1.5.sp),
        color = if (inverted) Palette.paper else Palette.ink,
        maxLines = 1,
        modifier = modifier
            .background(if (inverted) Palette.ink else Palette.paper)
            .then(if (inverted) Modifier else Modifier.einkBorder())
            .padding(
                horizontal = if (inverted) 8.dp else 6.dp,
                vertical = if (inverted) 4.dp else 2.dp,
            ),
    )
}

// MARK: - Progress

private val LocalReduceMotion: Boolean
    @Composable get() {
        val ctx = LocalContext.current
        return remember {
            runCatching {
                android.provider.Settings.Global.getFloat(
                    ctx.contentResolver, android.provider.Settings.Global.ANIMATOR_DURATION_SCALE, 1f,
                ) == 0f
            }.getOrDefault(false)
        }
    }

/**
 * Indeterminate progress as a row of square blocks filling in turn — the
 * e-ink stand-in for a spinner.
 */
@Composable
fun BlockSpinner(
    modifier: Modifier = Modifier,
    color: Color = Palette.ink,
    count: Int = 3,
    size: Dp = 8.dp,
) {
    val still = LocalReduceMotion
    var step by remember { mutableIntStateOf(if (still) count else 0) }
    if (!still) {
        LaunchedEffect(count) {
            while (true) {
                delay(350)
                step = (step + 1) % (count + 1)
            }
        }
    }
    Row(
        modifier.semantics { contentDescription = "In progress" },
        horizontalArrangement = Arrangement.spacedBy(3.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        repeat(count) { i ->
            Box(
                Modifier
                    .size(size)
                    .background(if (i < step) color else Color.Transparent)
                    .border(1.5.dp, color),
            )
        }
    }
}

/** Determinate progress as segmented blocks. [value] is 0..1. */
@Composable
fun BlockProgressBar(
    value: Float,
    modifier: Modifier = Modifier,
    segments: Int = 10,
    height: Dp = 10.dp,
    color: Color = Palette.ink,
) {
    val filled = (value.coerceIn(0f, 1f) * segments).toInt()
    Row(
        modifier.semantics {
            contentDescription = "Progress"
            stateDescription = "${(value * 100).toInt()} percent"
        },
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        repeat(segments) { i ->
            Box(
                Modifier
                    .weight(1f)
                    .height(height)
                    .background(if (i < filled) color else Color.Transparent)
                    .border(1.5.dp, color),
            )
        }
    }
}

// MARK: - Buttons

/** Full-width primary action — a solid black block, white CAPS in Anton. */
@Composable
fun PrimaryButton(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Row(
        modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 52.dp)
            .background(if (enabled) Palette.ink else Palette.faint)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) Icon(icon, contentDescription = null, tint = Palette.accentInk, modifier = Modifier.size(18.dp))
        Text(
            title.uppercase(),
            style = anton(20.sp).copy(letterSpacing = 2.sp),
            color = Palette.accentInk,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/**
 * Full-width secondary action — [PrimaryButton]'s block, outlined rather than
 * filled, so the eye still lands on the primary one first.
 */
@Composable
fun SecondaryButton(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val tint = if (enabled) Palette.ink else Palette.faint
    Row(
        modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 44.dp)
            .background(Palette.paper)
            .einkBorder(color = tint)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(16.dp))
        Text(title.uppercase(), style = caps(13.sp), color = tint, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** A compact CAPS button: black (primary) or outlined, 44dp tall. */
@Composable
fun BlockButton(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    filled: Boolean = true,
    /** Fill the available width (a full-width secondary action). */
    expand: Boolean = false,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val fg = if (filled) Palette.paper else Palette.ink
    Row(
        modifier
            .then(if (expand) Modifier.fillMaxWidth() else Modifier)
            .heightIn(min = 44.dp)
            .alpha(if (enabled) 1f else 0.4f)
            .background(if (filled) Palette.ink else Palette.paper)
            .then(if (filled) Modifier else Modifier.einkBorder())
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) Icon(icon, contentDescription = null, tint = fg, modifier = Modifier.size(16.dp))
        Text(title.uppercase(), style = caps(13.sp), color = fg, maxLines = 1)
    }
}

/** A 44dp square icon button with a 2dp border (back, import, recenter). */
@Composable
fun SquareIconButton(
    icon: ImageVector,
    label: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    filled: Boolean = false,
    onClick: () -> Unit,
) {
    val edge = if (enabled) Palette.ink else Palette.faint
    Box(
        modifier
            .size(44.dp)
            .background(if (filled) Palette.ink else Palette.paper)
            .einkBorder(color = edge)
            .clickable(enabled = enabled, role = Role.Button, onClickLabel = label, onClick = onClick)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon, contentDescription = null,
            tint = if (filled) Palette.paper else edge,
            modifier = Modifier.size(20.dp),
        )
    }
}

/** Small "Will sync when connected" note for settings changed in the app that
 *  the device hasn't taken yet. */
@Composable
fun PendingSyncHint(text: String, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth()) {
        Row(
            Modifier
                .background(Palette.paper)
                .einkBorder()
                .padding(horizontal = 8.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Icon(Icons.Filled.Sync, contentDescription = null, tint = Palette.ink, modifier = Modifier.size(13.dp))
            Text(text.uppercase(), style = caps(11.sp, 1.2.sp), color = Palette.ink)
        }
    }
}

// MARK: - Two-segment switch

/**
 * A choice as square segments, the chosen one inverted (KM | MI, ON | OFF).
 * Sized to its labels; [Segmented] is the full-width variant.
 */
@Composable
fun <T> SegmentedSwitch(
    options: List<Pair<String, T>>,
    selection: T,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    accessibilityName: String = "",
    onSelect: (T) -> Unit,
) {
    Row(
        modifier
            .height(IntrinsicSize.Min)
            .alpha(if (enabled) 1f else 0.4f)
            .einkBorder(),
    ) {
        options.forEachIndexed { i, (label, value) ->
            val on = value == selection
            if (i > 0) Box(Modifier.width(Rule.border).fillMaxHeight().background(Palette.ink))
            Box(
                Modifier
                    .defaultMinSize(minWidth = 44.dp, minHeight = 32.dp)
                    .background(if (on) Palette.ink else Palette.paper)
                    .clickable(enabled = enabled, role = Role.RadioButton) { onSelect(value) }
                    .semantics {
                        selected = on
                        contentDescription = if (accessibilityName.isEmpty()) label else "$accessibilityName, $label"
                    }
                    .padding(horizontal = 10.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label.uppercase(), style = caps(12.sp, 1.5.sp), maxLines = 1,
                    color = if (on) Palette.paper else Palette.ink,
                )
            }
        }
    }
}

/**
 * Every on/off setting in the app as an ON | OFF two-segment switch, in place
 * of Material's Switch (same parameters, so a call site swaps by name).
 */
@Composable
fun EinkSwitch(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    SegmentedSwitch(
        options = listOf("On" to true, "Off" to false),
        selection = checked,
        modifier = modifier.semantics { stateDescription = if (checked) "On" else "Off" },
        enabled = enabled && onCheckedChange != null,
    ) { onCheckedChange?.invoke(it) }
}

/**
 * The segmented picker, full width: square segments in a 2dp frame, the
 * chosen one inverted.
 */
@Composable
fun Segmented(
    options: List<String>,
    selected: Int,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onSelect: (Int) -> Unit,
) {
    Row(
        modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .alpha(if (enabled) 1f else 0.4f)
            .einkBorder(),
    ) {
        options.forEachIndexed { index, label ->
            val on = index == selected
            if (index > 0) Box(Modifier.width(Rule.border).fillMaxHeight().background(Palette.ink))
            Box(
                Modifier
                    .weight(1f)
                    .defaultMinSize(minHeight = 40.dp)
                    .background(if (on) Palette.ink else Palette.paper)
                    .clickable(enabled = enabled, role = Role.RadioButton) { onSelect(index) }
                    .semantics { this.selected = on }
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label.uppercase(), style = caps(12.sp, 1.2.sp), maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    color = if (on) Palette.paper else Palette.ink,
                )
            }
        }
    }
}

/** A checkmark tag for done rows in lists: "✓" in an inverted square. */
@Composable
fun DoneMark(modifier: Modifier = Modifier) {
    Box(modifier.size(18.dp).background(Palette.ink), contentAlignment = Alignment.Center) {
        Icon(Icons.Filled.Check, contentDescription = null, tint = Palette.paper, modifier = Modifier.size(14.dp))
    }
}

// MARK: - Material theme

/**
 * Material's colours, all ink and paper, so components this app doesn't draw
 * itself (dialogs, menus, text fields, sheets, chips) carry no stray colour.
 */
private val EinkScheme = lightColorScheme(
    primary = Palette.ink,
    onPrimary = Palette.paper,
    primaryContainer = Palette.accentWash,
    onPrimaryContainer = Palette.ink,
    inversePrimary = Palette.paper,
    secondary = Palette.ink,
    onSecondary = Palette.paper,
    secondaryContainer = Palette.accentWash,
    onSecondaryContainer = Palette.ink,
    tertiary = Palette.ink,
    onTertiary = Palette.paper,
    tertiaryContainer = Palette.accentWash,
    onTertiaryContainer = Palette.ink,
    background = Palette.paper,
    onBackground = Palette.ink,
    surface = Palette.surface,
    onSurface = Palette.ink,
    surfaceVariant = Palette.accentWash,
    onSurfaceVariant = Palette.muted,
    surfaceTint = Color.Transparent,
    inverseSurface = Palette.ink,
    inverseOnSurface = Palette.paper,
    error = Palette.ink,
    onError = Palette.paper,
    errorContainer = Palette.accentWash,
    onErrorContainer = Palette.ink,
    outline = Palette.ink,
    outlineVariant = Palette.faint,
    scrim = Color.Black.copy(alpha = 0.32f),
    surfaceBright = Palette.paper,
    surfaceDim = Palette.accentWash,
    surfaceContainer = Palette.paper,
    surfaceContainerHigh = Palette.paper,
    surfaceContainerHighest = Palette.paper,
    surfaceContainerLow = Palette.paper,
    surfaceContainerLowest = Palette.paper,
)

/** Square everything: dialogs, sheets, menus, text fields and buttons. */
private val SquareShapes = Shapes(
    extraSmall = CutCornerShape(0.dp),
    small = CutCornerShape(0.dp),
    medium = CutCornerShape(0.dp),
    large = CutCornerShape(0.dp),
    extraLarge = CutCornerShape(0.dp),
)

/**
 * The head unit is a paper-white device and the app is its paper-white
 * companion; a dark scheme would be a second design, not a variant of this
 * one. So the palette is fixed, exactly as on iOS.
 */
@Composable
fun OpenTrailPaperTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = EinkScheme,
        shapes = SquareShapes,
        typography = MaterialTheme.typography.copy(
            bodyLarge = TypeScale.body,
            bodyMedium = barlow(14.sp),
            bodySmall = barlow(12.sp),
            labelLarge = barlow(15.sp, FontWeight.Bold),
            labelMedium = barlow(12.sp, FontWeight.Bold),
            titleMedium = TypeScale.title,
            titleLarge = condensed(26.sp, FontWeight.Bold),
            headlineSmall = condensed(26.sp, FontWeight.Bold),
        ),
        content = content,
    )
}
