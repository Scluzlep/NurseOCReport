package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.scluzlep.nurseocreport.AppState
import com.scluzlep.nurseocreport.PointUi
import com.scluzlep.nurseocreport.formatNumber2dp
import com.scluzlep.nurseocreport.formatNumberTrim
import kotlin.math.hypot
import kotlin.math.min

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun WorkloadTab(
    state: AppState,
    onTakePhoto: () -> Unit,
    onPickPhoto: () -> Unit,
    onImportNames: () -> Unit,
) {
    var showWeekDetail by remember { mutableStateOf(false) }
    var showCustomInputDialog by remember { mutableStateOf(false) }
    var yearStr by remember(state.year) { mutableStateOf(state.year.toString()) }
    var monthStr by remember(state.month) { mutableStateOf(state.month.toString()) }

    val currentSheet = state.sheet
    val currentSummary = state.summary
    val selRow = state.selectedRow
    val selCol = state.selectedColIndex

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        FlowRow(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Button(onClick = onTakePhoto, enabled = !state.busy) {
                Text("拍照识别")
            }
            OutlinedButton(onClick = onPickPhoto, enabled = !state.busy) {
                Text("相册选图")
            }
            OutlinedButton(
                onClick = { state.rotateWorkload(3) },
                enabled = !state.busy && currentSheet != null,
            ) {
                Text("向左转")
            }
            OutlinedButton(
                onClick = { state.rotateWorkload(1) },
                enabled = !state.busy && currentSheet != null,
            ) {
                Text("向右转")
            }
            OutlinedButton(
                onClick = { state.toggleCornerEditMode() },
                enabled = !state.busy && currentSheet != null,
            ) {
                Text(if (state.cornerEditMode) "取消框选" else "四角微调")
            }
            if (state.cornerEditMode) {
                Button(onClick = { state.applyAdjustedCorners() }, enabled = !state.busy) {
                    Text("按新框重算")
                }
            }
            OutlinedButton(
                onClick = onImportNames,
                enabled = !state.busy && currentSheet != null,
            ) {
                Text("导入名单")
            }
            Button(
                onClick = { state.exportAndShareXlsx() },
                enabled = !state.busy && currentSheet != null,
            ) {
                Text("导出分享")
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = yearStr,
                onValueChange = { txt ->
                    yearStr = txt
                    txt.toIntOrNull()?.let { y -> if (y in 2000..2100) state.year = y }
                },
                label = { Text("年") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.width(88.dp),
            )
            OutlinedTextField(
                value = monthStr,
                onValueChange = { txt ->
                    monthStr = txt
                    txt.toIntOrNull()?.let { m -> if (m in 1..12) state.month = m }
                },
                label = { Text("月") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.width(72.dp),
            )
            OutlinedButton(
                onClick = { state.selectNextPendingReview() },
                enabled = currentSheet != null,
            ) {
                Text("下一处待确认")
            }
            OutlinedButton(
                onClick = { showWeekDetail = !showWeekDetail },
                enabled = currentSheet != null,
            ) {
                Text(if (showWeekDetail) "汇总视图" else "周明细")
            }
        }

        // Photo preview & cell highlight canvas
        val bmp = state.workingBitmap
        if (bmp != null && currentSheet != null) {
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(210.dp),
            ) {
                InteractiveWorkloadCanvas(
                    state = state,
                    bitmap = bmp,
                )
            }
        }

        // Selected cell verification & 1-tap quick fill panel
        if (currentSheet != null && selRow in currentSheet.rows.indices) {
            val rowObj = currentSheet.rows[selRow]
            val titleLabel = if (selCol < 0) {
                "第${selRow + 1}行 姓名: ${rowObj.staffName}"
            } else if (selCol in rowObj.cells.indices) {
                val cell = rowObj.cells[selCol]
                val sName = currentSheet.columnShiftNames.getOrNull(cell.shiftCol) ?: "列${cell.shiftCol + 1}"
                val valStr = if (cell.isBlank) "空白" else formatNumberTrim(cell.value)
                "${rowObj.staffName} 第${cell.week + 1}周 $sName = $valStr"
            } else {
                ""
            }

            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(
                    modifier = Modifier.padding(8.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        val cropBmp = state.cellPreviewBitmap
                        if (cropBmp != null) {
                            Image(
                                bitmap = cropBmp.asImageBitmap(),
                                contentDescription = "局部截图",
                                contentScale = ContentScale.Fit,
                                modifier = Modifier
                                    .width(96.dp)
                                    .height(48.dp)
                                    .background(Color.White),
                            )
                        }
                        Column(modifier = Modifier.weight(1f)) {
                            Text(titleLabel, fontWeight = FontWeight.Bold)
                            if (selCol >= 0 && selCol in rowObj.cells.indices && rowObj.cells[selCol].reviewReason.isNotBlank()) {
                                Text(
                                    rowObj.cells[selCol].reviewReason,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = Color(0xFF8A4B00),
                                )
                            }
                        }
                        TextButton(onClick = { showCustomInputDialog = true }) {
                            Text("手输")
                        }
                    }

                    if (selCol >= 0) {
                        FlowRow(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            FilledTonalButton(onClick = { state.editCell(selRow, selCol, true, 0.0, null) }) {
                                Text("空白")
                            }
                            listOf("1" to 1.0, "2" to 2.0, "3" to 3.0, "4" to 4.0, "5" to 5.0, "0.5" to 0.5).forEach { (label, num) ->
                                FilledTonalButton(onClick = { state.editCell(selRow, selCol, false, num, null) }) {
                                    Text(label)
                                }
                            }
                            Button(onClick = { state.confirmAndAdvance() }) {
                                Text("确认")
                            }
                        }
                    } else {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(onClick = { showCustomInputDialog = true }) {
                                Text("修改姓名")
                            }
                            Button(onClick = { state.confirmAndAdvance() }) {
                                Text("确认")
                            }
                        }
                    }
                }
            }
        }

        // Editable workload summary or weekly cells grid
        Card(modifier = Modifier.weight(1f).fillMaxWidth()) {
            if (currentSheet == null || currentSummary == null) {
                Box(
                    modifier = Modifier.fillMaxSize().padding(18.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        "请先拍照或从相册选择整月工作量表照片。",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            } else if (!showWeekDetail) {
                SummaryTableSection(
                    state = state,
                    sheet = currentSheet,
                    summary = currentSummary,
                    onEditName = { rowIdx ->
                        state.selectCell(rowIdx, -1)
                        showCustomInputDialog = true
                    },
                )
            } else {
                WeeklyDetailSection(
                    state = state,
                    sheet = currentSheet,
                )
            }
        }
    }

    if (showCustomInputDialog && currentSheet != null && selRow in currentSheet.rows.indices) {
        val rowObj = currentSheet.rows[selRow]
        val isName = selCol < 0
        val initVal = if (isName) {
            rowObj.staffName
        } else if (selCol in rowObj.cells.indices && !rowObj.cells[selCol].isBlank) {
            formatNumberTrim(rowObj.cells[selCol].value)
        } else {
            ""
        }
        var inputText by remember(selRow, selCol) { mutableStateOf(initVal) }

        AlertDialog(
            onDismissRequest = { showCustomInputDialog = false },
            title = { Text(if (isName) "修改姓名" else "输入数值") },
            text = {
                OutlinedTextField(
                    value = inputText,
                    onValueChange = { inputText = it },
                    keyboardOptions = KeyboardOptions(
                        keyboardType = if (isName) KeyboardType.Text else KeyboardType.Decimal
                    ),
                    singleLine = true,
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (isName) {
                            if (inputText.isNotBlank()) {
                                state.editCell(selRow, -1, false, 0.0, inputText.trim())
                            }
                        } else {
                            val trimmed = inputText.trim()
                            if (trimmed.isEmpty()) {
                                state.editCell(selRow, selCol, true, 0.0, null)
                            } else {
                                trimmed.toDoubleOrNull()?.let { v ->
                                    state.editCell(selRow, selCol, false, v, null)
                                }
                            }
                        }
                        showCustomInputDialog = false
                    }
                ) {
                    Text("保存")
                }
            },
            dismissButton = {
                TextButton(onClick = { showCustomInputDialog = false }) {
                    Text("取消")
                }
            },
        )
    }
}

@Composable
private fun InteractiveWorkloadCanvas(
    state: AppState,
    bitmap: android.graphics.Bitmap,
) {
    val sheet = state.sheet ?: return
    val srcW = maxOf(1, sheet.imgWidth.takeIf { it > 0 } ?: bitmap.width)
    val srcH = maxOf(1, sheet.imgHeight.takeIf { it > 0 } ?: bitmap.height)
    val imgBitmap = remember(bitmap) { bitmap.asImageBitmap() }

    var canvasSize by remember { mutableStateOf(IntSize(1, 1)) }
    var scale by remember { mutableFloatStateOf(1f) }
    var panX by remember { mutableFloatStateOf(0f) }
    var panY by remember { mutableFloatStateOf(0f) }
    var dragCornerIdx by remember { mutableIntStateOf(-1) }

    LaunchedEffect(bitmap, canvasSize) {
        if (canvasSize.width > 1 && canvasSize.height > 1) {
            val sx = canvasSize.width.toFloat() / srcW.toFloat()
            val sy = canvasSize.height.toFloat() / srcH.toFloat()
            scale = min(sx, sy)
            panX = (canvasSize.width - srcW * scale) * 0.5f
            panY = (canvasSize.height - srcH * scale) * 0.5f
        }
    }

    val modifier = Modifier
        .fillMaxSize()
        .clipToBounds()
        .background(Color(0xFF222222))
        .onSizeChanged { canvasSize = it }
        .then(
            if (state.cornerEditMode) {
                Modifier.pointerInput("cornerDrag", state.cornerEditMode, state.editableCorners.size) {
                    detectDragGestures(
                        onDragStart = { start ->
                            val ix = (start.x - panX) / scale
                            val iy = (start.y - panY) / scale
                            var bestIdx = -1
                            var bestDist = 48f / maxOf(0.2f, scale)
                            for (i in 0 until state.editableCorners.size) {
                                val p = state.editableCorners[i]
                                val d = hypot(p.x - ix, p.y - iy)
                                if (d < bestDist) {
                                    bestDist = d
                                    bestIdx = i
                                }
                            }
                            dragCornerIdx = bestIdx
                        },
                        onDrag = { change, dragAmount ->
                            change.consume()
                            if (dragCornerIdx >= 0 && dragCornerIdx < state.editableCorners.size) {
                                val cur = state.editableCorners[dragCornerIdx]
                                state.moveCorner(
                                    dragCornerIdx,
                                    PointUi(cur.x + dragAmount.x / scale, cur.y + dragAmount.y / scale),
                                )
                            } else {
                                panX += dragAmount.x
                                panY += dragAmount.y
                            }
                        },
                        onDragEnd = { dragCornerIdx = -1 },
                    )
                }
            } else {
                Modifier
                    .pointerInput("tapCell", sheet) {
                        detectTapGestures { offset ->
                            val ix = (offset.x - panX) / scale
                            val iy = (offset.y - panY) / scale
                            for (rIdx in sheet.rows.indices) {
                                val row = sheet.rows[rIdx]
                                if (pointInQuad(ix, iy, row.nameQuad)) {
                                    state.selectCell(rIdx, -1)
                                    return@detectTapGestures
                                }
                                for (cIdx in row.cells.indices) {
                                    if (pointInQuad(ix, iy, row.cells[cIdx].quad)) {
                                        state.selectCell(rIdx, cIdx)
                                        return@detectTapGestures
                                    }
                                }
                            }
                        }
                    }
                    .pointerInput("panZoom") {
                        detectTransformGestures { centroid, pan, zoom, _ ->
                            val oldScale = scale
                            val newScale = (scale * zoom).coerceIn(0.25f, 6.0f)
                            val factor = newScale / oldScale
                            panX = (panX - centroid.x) * factor + centroid.x + pan.x
                            panY = (panY - centroid.y) * factor + centroid.y + pan.y
                            scale = newScale
                        }
                    }
            }
        )

    Canvas(modifier = modifier) {
        withTransform({
            translate(left = panX, top = panY)
            scale(scaleX = scale, scaleY = scale, pivot = Offset.Zero)
        }) {
            drawImage(
                image = imgBitmap,
                srcOffset = IntOffset.Zero,
                srcSize = IntSize(bitmap.width, bitmap.height),
                dstOffset = IntOffset.Zero,
                dstSize = IntSize(srcW, srcH),
            )

            val borderStroke = Stroke(width = maxOf(1.5f, 2.0f / scale))
            val selStroke = Stroke(width = maxOf(2.5f, 3.5f / scale))

            // Draw NeedsReview highlights and selected cell highlight
            for (rIdx in sheet.rows.indices) {
                val row = sheet.rows[rIdx]
                if (row.nameQuad.size == 4) {
                    val isSel = (rIdx == state.selectedRow && state.selectedColIndex == -1)
                    if (isSel || row.nameNeedsReview) {
                        val path = quadToPath(row.nameQuad)
                        drawPath(
                            path = path,
                            color = if (isSel) Color(0xFF1976D2) else Color(0xFFFF9800),
                            style = if (isSel) selStroke else borderStroke,
                        )
                    }
                }
                for (cIdx in row.cells.indices) {
                    val cell = row.cells[cIdx]
                    if (cell.quad.size != 4) continue
                    val isSel = (rIdx == state.selectedRow && cIdx == state.selectedColIndex)
                    val needRev = (cell.status == 1)
                    if (isSel || needRev) {
                        val path = quadToPath(cell.quad)
                        drawPath(
                            path = path,
                            color = if (isSel) Color(0xFF1976D2) else Color(0xFFFF9800),
                            style = if (isSel) selStroke else borderStroke,
                        )
                    }
                }
            }

            if (state.cornerEditMode && state.editableCorners.size == 4) {
                val poly = quadToPath(state.editableCorners)
                drawPath(poly, color = Color(0xFF00E676), style = selStroke)
                val radius = 14f / maxOf(0.2f, scale)
                for (pt in state.editableCorners) {
                    drawCircle(
                        color = Color(0xFF00E676),
                        radius = radius,
                        center = Offset(pt.x, pt.y),
                    )
                }
            }
        }
    }
}

@Composable
private fun SummaryTableSection(
    state: AppState,
    sheet: com.scluzlep.nurseocreport.WorkloadSheetUi,
    summary: com.scluzlep.nurseocreport.SheetSummaryUi,
    onEditName: (Int) -> Unit,
) {
    val hScroll = rememberScrollState()
    val enabledRules = state.shiftRules
    Column(modifier = Modifier.fillMaxSize().horizontalScroll(hScroll)) {
        Row(
            modifier = Modifier
                .background(Color(0xFFD9EAF7))
                .padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("姓名", modifier = Modifier.width(84.dp), fontWeight = FontWeight.Bold)
            enabledRules.forEach { r ->
                Text(r.name, modifier = Modifier.width(68.dp), fontWeight = FontWeight.Bold)
            }
            Text("加权合计", modifier = Modifier.width(76.dp), fontWeight = FontWeight.Bold)
            Text("系数", modifier = Modifier.width(56.dp), fontWeight = FontWeight.Bold)
            Text("最终工作量", modifier = Modifier.width(88.dp), fontWeight = FontWeight.Bold)
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f)) {
            itemsIndexed(summary.people) { idx, person ->
                val needsRev = sheet.rows.getOrNull(idx)?.nameNeedsReview == true
                Row(
                    modifier = Modifier
                        .background(
                            when {
                                idx == state.selectedRow && state.selectedColIndex == -1 -> Color(0xFFE3F2FD)
                                needsRev || !person.matchedInStaff -> Color(0xFFFFF3CD)
                                idx % 2 == 1 -> Color(0xFFF8FAFD)
                                else -> Color.White
                            }
                        )
                        .clickable { onEditName(idx) }
                        .padding(horizontal = 8.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(person.name, modifier = Modifier.width(84.dp), fontWeight = FontWeight.Medium)
                    enabledRules.indices.forEach { rIdx ->
                        val v = person.shiftTotals.getOrElse(rIdx) { 0.0 }
                        Text(formatNumberTrim(v), modifier = Modifier.width(68.dp))
                    }
                    Text(formatNumber2dp(person.weightedSum), modifier = Modifier.width(76.dp))
                    Text(formatNumber2dp(person.effectiveCoeff), modifier = Modifier.width(56.dp))
                    Text(
                        formatNumber2dp(person.finalScore),
                        modifier = Modifier.width(88.dp),
                        fontWeight = FontWeight.Bold,
                    )
                }
                HorizontalDivider()
            }
            item {
                Row(
                    modifier = Modifier
                        .background(Color(0xFFE7E6E6))
                        .padding(horizontal = 8.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("合计", modifier = Modifier.width(84.dp), fontWeight = FontWeight.Bold)
                    enabledRules.indices.forEach { rIdx ->
                        val gt = summary.shiftGrandTotals.getOrElse(rIdx) { 0.0 }
                        Text(formatNumberTrim(gt), modifier = Modifier.width(68.dp), fontWeight = FontWeight.Bold)
                    }
                    Text(formatNumber2dp(summary.weightedGrandTotal), modifier = Modifier.width(76.dp), fontWeight = FontWeight.Bold)
                    Text("-", modifier = Modifier.width(56.dp))
                    Text(formatNumber2dp(summary.finalGrandTotal), modifier = Modifier.width(88.dp), fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

@Composable
private fun WeeklyDetailSection(
    state: AppState,
    sheet: com.scluzlep.nurseocreport.WorkloadSheetUi,
) {
    val hScroll = rememberScrollState()
    val colsPerWeek = sheet.columnShiftNames.size
    Column(modifier = Modifier.fillMaxSize().horizontalScroll(hScroll)) {
        Row(
            modifier = Modifier
                .background(Color(0xFFD9EAF7))
                .padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("姓名", modifier = Modifier.width(80.dp), fontWeight = FontWeight.Bold)
            for (w in 0 until sheet.weeks) {
                for (sc in 0 until colsPerWeek) {
                    Text(
                        "W${w + 1}${sheet.columnShiftNames[sc]}",
                        modifier = Modifier.width(58.dp),
                        fontWeight = FontWeight.Bold,
                    )
                }
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f)) {
            itemsIndexed(sheet.rows) { rIdx, row ->
                Row(
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        row.staffName,
                        modifier = Modifier
                            .width(80.dp)
                            .clickable { state.selectCell(rIdx, -1) },
                        fontWeight = FontWeight.Medium,
                    )
                    row.cells.forEachIndexed { cIdx, cell ->
                        val isSel = (rIdx == state.selectedRow && cIdx == state.selectedColIndex)
                        val bg = when {
                            isSel -> Color(0xFFBBDEFB)
                            cell.status == 1 -> Color(0xFFFFF3CD)
                            cell.status == 2 -> Color(0xFFE8F5E9)
                            else -> Color.Transparent
                        }
                        Box(
                            modifier = Modifier
                                .width(58.dp)
                                .background(bg)
                                .clickable { state.selectCell(rIdx, cIdx) }
                                .padding(vertical = 6.dp, horizontal = 4.dp),
                        ) {
                            val txt = if (cell.isBlank) "" else formatNumberTrim(cell.value)
                            Text(txt.ifEmpty { "-" })
                        }
                    }
                }
                HorizontalDivider()
            }
        }
    }
}

private fun quadToPath(pts: List<PointUi>): Path {
    val p = Path()
    if (pts.size < 4) return p
    p.moveTo(pts[0].x, pts[0].y)
    p.lineTo(pts[1].x, pts[1].y)
    p.lineTo(pts[2].x, pts[2].y)
    p.lineTo(pts[3].x, pts[3].y)
    p.close()
    return p
}

private fun pointInQuad(px: Float, py: Float, quad: List<PointUi>): Boolean {
    if (quad.size != 4) return false
    var pos = 0
    var neg = 0
    for (i in 0 until 4) {
        val a = quad[i]
        val b = quad[(i + 1) % 4]
        val cross = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)
        if (cross > 0f) pos++
        else if (cross < 0f) neg++
    }
    return pos == 0 || neg == 0
}
