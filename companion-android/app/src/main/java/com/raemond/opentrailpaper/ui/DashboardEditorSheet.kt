package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DragHandle
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.DashField
import com.raemond.opentrailpaper.data.DashItem
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import kotlin.math.roundToInt

/**
 * Rearrange the head unit's dashboard from the phone.
 *
 * The preview is the point. A list of field names cannot tell you that four
 * mediums leave everything small, or that a lone `half` field silently spans the
 * row — so the editor draws what the panel will draw, using the same packing
 * rules and height weights as ui_render.cpp.
 */
@Composable
fun DashboardEditorSheet(ble: BleManager, onDismiss: () -> Unit) {
    var layout by remember { mutableStateOf(ble.dashLayout ?: DashLayout(emptyList())) }
    var dirty by remember { mutableStateOf(false) }
    var showAdd by remember { mutableStateOf(false) }

    // The device is the source of truth: if it corrects or rejects what we sent,
    // adopt what it actually holds rather than keeping a local fiction on screen.
    // Skipped while the rider has unsent edits, so a stray notify can't wipe work
    // in progress.
    LaunchedEffect(ble.dashLayout) {
        val fromDevice = ble.dashLayout
        if (!dirty && fromDevice != null) layout = fromDevice
    }

    FullScreenSheet(
        title = "Dashboard",
        onDismiss = onDismiss,
        trailing = {
            TextButton(
                // Enabled whenever this layout is not what the device is holding
                // — a comparison, not a flag someone has to remember to set. A
                // flag clears itself the moment Send is tapped, so if the write
                // never landed the button was dead and the only way to retry was
                // to close the sheet and edit something.
                enabled = layout.items.isNotEmpty() && layout != ble.dashLayout,
                onClick = {
                    ble.sendDashLayout(layout)
                    dirty = false
                },
            ) {
                Text("Send", style = TypeScale.bodyStrong)
            }
        },
    ) {
        if (ble.dashLayout == null) {
            Box(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                Column(
                    Modifier.padding(32.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text("Connect to edit", style = TypeScale.title, color = Palette.ink)
                    Text(
                        "The layout is read from the device, so it can't be edited until " +
                            "the head unit is connected.",
                        style = TypeScale.body,
                        color = Palette.muted,
                    )
                }
            }
            return@FullScreenSheet
        }

        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            TrackedLabel("How the panel will look")
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                DashPreview(
                    layout,
                    Modifier
                        .height(320.dp)
                        .aspectRatio(DASH_PANEL_ASPECT),
                )
            }

            TrackedLabel("Fields")
            ReorderableItems(
                items = layout.items,
                onMove = { from, to ->
                    layout = DashLayout(layout.items.toMutableList().also {
                        it.add(to, it.removeAt(from))
                    })
                    dirty = true
                },
                onChange = { index, item ->
                    layout = DashLayout(layout.items.toMutableList().also { it[index] = item })
                    dirty = true
                },
                onDelete = { index ->
                    layout = DashLayout(layout.items.toMutableList().also { it.removeAt(index) })
                    dirty = true
                },
            )

            if (layout.items.size < DashLayout.MAX_ITEMS) {
                TextButton(onClick = { showAdd = true }) {
                    Icon(Icons.Filled.Add, contentDescription = null, tint = Palette.accent)
                    Spacer(Modifier.size(6.dp))
                    Text("Add a field", style = TypeScale.bodyStrong, color = Palette.accent)
                }
            }

            Text(
                "Long-press the handle to reorder. “Half width” pairs a field with the next " +
                    "half-width field; on its own it spans the row.",
                style = barlow(12.sp),
                color = Palette.muted,
            )
            Text(
                "Fields that need a sensor — power, heart rate, cadence — are hidden on the " +
                    "device until it connects, and the rest expand to fill the space. Route " +
                    "left hides when no route is loaded.",
                style = barlow(12.sp),
                color = Palette.muted,
            )

            HorizontalDivider(color = Palette.hairline)
            TextButton(onClick = {
                layout = DashLayout.deviceDefault
                dirty = true
            }) {
                Text(
                    "Reset to the default layout",
                    style = TypeScale.bodyStrong,
                    color = Palette.accent,
                )
            }
        }
    }

    if (showAdd) {
        FieldPickerSheet(
            onDismiss = { showAdd = false },
            onPick = { id ->
                layout = DashLayout(layout.items + DashItem(id, DashSize.MEDIUM, true))
                dirty = true
                showAdd = false
            },
        )
    }
}

/**
 * The field rows, reorderable by long-pressing the handle.
 *
 * A plain Column rather than a reorderable list library: the layout is capped at
 * 12 items, so there is nothing to virtualise, and the drag maths reduces to
 * "how many row heights has the finger travelled".
 */
@Composable
private fun ReorderableItems(
    items: List<DashItem>,
    onMove: (Int, Int) -> Unit,
    onChange: (Int, DashItem) -> Unit,
    onDelete: (Int) -> Unit,
) {
    var dragIndex by remember { mutableIntStateOf(-1) }
    var dragOffset by remember { mutableFloatStateOf(0f) }
    var rowHeight by remember { mutableFloatStateOf(1f) }

    Column {
        items.forEachIndexed { index, item ->
            val dragging = index == dragIndex
            Box(
                Modifier
                    .onSizeChanged { if (it.height > 0) rowHeight = it.height.toFloat() }
                    .offset { IntOffset(0, if (dragging) dragOffset.roundToInt() else 0) }
                    .alpha(if (dragging) 0.85f else 1f),
            ) {
                DashItemRow(
                    item = item,
                    onChange = { onChange(index, it) },
                    onDelete = { onDelete(index) },
                    dragModifier = Modifier.pointerInput(index, items.size) {
                        detectDragGesturesAfterLongPress(
                            onDragStart = { dragIndex = index; dragOffset = 0f },
                            onDragEnd = {
                                val shift = (dragOffset / rowHeight).roundToInt()
                                val target = (index + shift).coerceIn(0, items.size - 1)
                                if (target != index) onMove(index, target)
                                dragIndex = -1
                                dragOffset = 0f
                            },
                            onDragCancel = { dragIndex = -1; dragOffset = 0f },
                            onDrag = { change, amount ->
                                change.consume()
                                dragOffset += amount.y
                            },
                        )
                    },
                )
            }
        }
    }
}

/** One configurable row: field name, size, and whether it shares its row. */
@Composable
private fun DashItemRow(
    item: DashItem,
    onChange: (DashItem) -> Unit,
    onDelete: () -> Unit,
    dragModifier: Modifier,
) {
    Card(Modifier.padding(vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Filled.DragHandle,
                contentDescription = "Reorder",
                tint = Palette.faint,
                modifier = dragModifier.padding(end = 10.dp),
            )
            Text(
                item.fieldLabel,
                style = TypeScale.bodyStrong,
                color = Palette.ink,
                modifier = Modifier.weight(1f),
            )
            IconButton(onClick = onDelete) {
                Icon(Icons.Filled.Close, contentDescription = "Remove", tint = Palette.muted)
            }
        }
        Row(
            Modifier.padding(top = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            DashSize.entries.forEach { size ->
                SmallChip(size.label, item.size == size) { onChange(item.copy(size = size)) }
            }
            Spacer(Modifier.weight(1f))
            SmallChip("Half", item.half) { onChange(item.copy(half = !item.half)) }
        }
    }
}

@Composable
private fun SmallChip(label: String, selected: Boolean, onClick: () -> Unit) {
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = { Text(label, style = barlow(12.sp, FontWeight.SemiBold)) },
        shape = RoundedCornerShape(50),
        colors = FilterChipDefaults.filterChipColors(
            containerColor = Palette.paper,
            labelColor = Palette.muted,
            selectedContainerColor = Palette.accentWash,
            selectedLabelColor = Palette.accent,
        ),
    )
}

@Composable
private fun FieldPickerSheet(onDismiss: () -> Unit, onPick: (String) -> Unit) {
    FullScreenSheet(title = "Add a field", onDismiss = onDismiss, confirmLabel = "Cancel") {
        LazyColumn {
            items(DashField.all, key = { it.id }) { field ->
                Column(
                    Modifier
                        .fillMaxWidth()
                        .clickable { onPick(field.id) }
                        .padding(horizontal = 20.dp, vertical = 14.dp),
                ) {
                    Text(field.label, style = TypeScale.body, color = Palette.ink)
                    Text(field.detail, style = barlow(12.sp), color = Palette.muted)
                }
                HorizontalDivider(color = Palette.hairline)
            }
        }
    }
}
