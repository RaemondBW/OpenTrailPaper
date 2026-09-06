package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.R

// Design system for the OpenTrailPaper companion app — a straight port of
// companion-ios/Sources/Theme.swift, so the two phones look like one product.
//
// The language borrows from the e-paper head unit it pairs with: warm off-white
// "paper", near-black ink, heavy condensed numerals, generous tracking on small
// labels, and a single high-energy accent for live data and primary actions.

object Palette {
    val paper = Color(0xFFEDEAE1)      // app background (warm cream)
    val surface = Color(0xFFFAF8F3)    // cards (slightly lighter)
    val ink = Color(0xFF1A1A1A)        // primary text
    val muted = Color(0xFF5C564B)      // secondary text (darker warm gray, readable)
    val faint = Color(0xFF9A9488)      // placeholders / tertiary
    val hairline = Color(0xFFDBD6C6)   // separators / borders
    val accent = Color(0xFFF4501E)     // vermilion — actions, live
    val accentDark = Color(0xFFB9541E) // pressed / on-accent detail
    val accentWash = Color(0xFFFBE9E0) // selected chip / accent tint bg
    val accentInk = Color.White
    val good = Color(0xFF2E7D5B)       // connected / success
}

// Bundled Barlow: condensed for numerals/titles/labels (athletic, echoes the
// device's Impact numerals); regular Barlow for body copy.
val BarlowCondensed = FontFamily(
    Font(R.font.barlowcondensedmedium, FontWeight.Medium),
    Font(R.font.barlowcondensedsemibold, FontWeight.SemiBold),
    Font(R.font.barlowcondensedbold, FontWeight.Bold),
)

val Barlow = FontFamily(
    Font(R.font.barlowregular, FontWeight.Normal),
    Font(R.font.barlowmedium, FontWeight.Medium),
    Font(R.font.barlowsemibold, FontWeight.SemiBold),
)

fun condensed(size: TextUnit, weight: FontWeight = FontWeight.SemiBold) =
    TextStyle(fontFamily = BarlowCondensed, fontWeight = weight, fontSize = size)

fun barlow(size: TextUnit, weight: FontWeight = FontWeight.Normal) =
    TextStyle(fontFamily = Barlow, fontWeight = weight, fontSize = size)

object TypeScale {
    // Big numerals — heavy condensed, echoing the device hero numbers.
    fun hero(size: TextUnit = 68.sp) = condensed(size, FontWeight.Bold)
    fun value(size: TextUnit = 30.sp) = condensed(size, FontWeight.SemiBold)
    val screenTitle = condensed(38.sp, FontWeight.Bold)
    val title = condensed(24.sp, FontWeight.SemiBold)
    val body = barlow(16.sp)
    val bodyStrong = barlow(15.sp, FontWeight.SemiBold)
    val label = condensed(13.sp, FontWeight.Medium)
}

/** Tracked-out uppercase label, matching the device's field captions. */
@Composable
fun TrackedLabel(text: String, modifier: Modifier = Modifier, color: Color = Palette.muted) {
    Text(
        text = text.uppercase(),
        modifier = modifier,
        style = TypeScale.label.copy(letterSpacing = 1.2.sp),
        color = color,
    )
}

/** A soft content card — the mockup's rounded, hairline-bordered surface. */
@Composable
fun Card(
    modifier: Modifier = Modifier,
    padding: androidx.compose.ui.unit.Dp = 16.dp,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Palette.surface, RoundedCornerShape(18.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(18.dp))
            .padding(padding),
        content = content,
    )
}

/** Full-width primary action — fully rounded vermilion pill. */
@Composable
fun PrimaryButton(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(26.dp),
        contentPadding = PaddingValues(vertical = 16.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = Palette.accent,
            contentColor = Palette.accentInk,
            disabledContainerColor = Palette.faint,
            disabledContentColor = Palette.accentInk,
        ),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (icon != null) Icon(icon, contentDescription = null)
            Text(title, style = condensed(21.sp, FontWeight.SemiBold))
        }
    }
}

/**
 * Full-width secondary action — [PrimaryButton]'s pill, outlined rather than
 * filled.
 *
 * Same geometry on purpose: an optional action stacked above a primary one has
 * to read as a button of the same family, or it reads as a link. Colour carries
 * the hierarchy instead — vermilion on paper rather than paper on vermilion —
 * so the eye still lands on the primary action first. Slightly shorter than
 * [PrimaryButton] for the same reason.
 */
@Composable
fun SecondaryButton(
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier
            .fillMaxWidth()
            .border(
                1.5.dp,
                if (enabled) Palette.accent else Palette.hairline,
                RoundedCornerShape(26.dp),
            ),
        shape = RoundedCornerShape(26.dp),
        contentPadding = PaddingValues(vertical = 13.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = Palette.surface,
            contentColor = Palette.accent,
            disabledContainerColor = Palette.surface,
            disabledContentColor = Palette.faint,
        ),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (icon != null) Icon(icon, contentDescription = null)
            Text(title, style = condensed(18.sp, FontWeight.SemiBold))
        }
    }
}

private val LightScheme = lightColorScheme(
    primary = Palette.accent,
    onPrimary = Palette.accentInk,
    secondary = Palette.good,
    background = Palette.paper,
    onBackground = Palette.ink,
    surface = Palette.surface,
    onSurface = Palette.ink,
    surfaceVariant = Palette.paper,
    onSurfaceVariant = Palette.muted,
    outline = Palette.hairline,
    error = Palette.accent,
)

/// The head unit is a paper-white device and the app is its paper-white
/// companion; a dark scheme would be a second design, not a variant of this one.
/// So the palette is fixed, exactly as on iOS.
@Composable
fun OpenTrailPaperTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = LightScheme,
        typography = MaterialTheme.typography.copy(
            bodyLarge = TypeScale.body,
            bodyMedium = barlow(14.sp),
            labelLarge = TypeScale.bodyStrong,
            titleMedium = TypeScale.title,
        ),
        content = content,
    )
}

/**
 * The iOS segmented picker, in the app's palette. Shared rather than duplicated:
 * `SegmentedButton` is an extension on the row's scope, so it can only be called
 * unqualified from inside one — which makes an ad-hoc copy per screen the only
 * other option.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Segmented(
    options: List<String>,
    selected: Int,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onSelect: (Int) -> Unit,
) {
    SingleChoiceSegmentedButtonRow(modifier.fillMaxWidth()) {
        options.forEachIndexed { index, label ->
            SegmentedButton(
                selected = index == selected,
                onClick = { onSelect(index) },
                enabled = enabled,
                shape = SegmentedButtonDefaults.itemShape(index, options.size),
                colors = SegmentedButtonDefaults.colors(
                    activeContainerColor = Palette.accentWash,
                    activeContentColor = Palette.accent,
                    activeBorderColor = Palette.accent,
                    inactiveContainerColor = Palette.surface,
                    inactiveContentColor = Palette.muted,
                    inactiveBorderColor = Palette.hairline,
                    // Backlight is disabled until the device is connected, and
                    // Material's untouched disabled colours are a lavender that
                    // belongs to no part of this palette.
                    disabledActiveContainerColor = Palette.paper,
                    disabledActiveContentColor = Palette.faint,
                    disabledActiveBorderColor = Palette.hairline,
                    disabledInactiveContainerColor = Palette.paper,
                    disabledInactiveContentColor = Palette.faint,
                    disabledInactiveBorderColor = Palette.hairline,
                ),
            ) {
                Text(label, style = barlow(13.sp, FontWeight.Medium), maxLines = 1)
            }
        }
    }
}
