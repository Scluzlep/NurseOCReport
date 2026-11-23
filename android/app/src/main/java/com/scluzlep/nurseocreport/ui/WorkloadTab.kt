package com.scluzlep.nurseocreport.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.scluzlep.nurseocreport.AppState
import com.scluzlep.nurseocreport.PointUi
import com.scluzlep.nurseocreport.WorkloadSheetUi
import com.scluzlep.nurseocreport.formatNumber2dp
import com.scluzlep.nurseocreport.formatNumberTrim
import kotlinx.coroutines.launch
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.CardDefaults
import top.yukonga.miuix.kmp.basic.HorizontalDivider
import top.yukonga.miuix.kmp.basic.Surface
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.extra.SuperDialog
import top.yukonga.miuix.kmp.theme.MiuixTheme
import java.util.Locale
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
    var showDeleteRowConfirm by remember { mutableStateOf(false) }
    var showAddRowDialog by remember { mutableStateOf(false) }
    var showYearMonthDialog by remember { mutableStateOf(false) }
    var showExportConfirmDialog by remember { mutableStateOf(false) }

    val currentSheet = state.sheet
    val currentSummary = state.summary
    val selRow = state.selectedRow
    val selCol = state.selectedColIndex
    val hasSelectedRow = currentSheet != null && selRow in currentSheet.rows.indices

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        ToolbarGridRow(
            slotCount = 4,
            content = listOf(
                {
                    UniformToolbarButton(
                        text = "拍照识别",
                        onClick = onTakePhoto,
                        enabled = !state.busy,
                        primary = true,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "相册选图",
                        onClick = onPickPhoto,
                        enabled = !state.busy,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "导入名单",
                        onClick = onImportNames,
                        enabled = !state.busy,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "导出分享",
                        onClick = {
                            if ((currentSummary?.pendingReviewCount ?: 0) > 0) {
                                showExportConfirmDialog = true
                            } else {
                                state.exportAndShareXlsx()
                            }
                        },
                        enabled = !state.busy && currentSheet != null && currentSheet.rows.isNotEmpty(),
                        primary = true,
                        modifier = Modifier.weight(1f),
                    )
                },
            ),
        )

        ToolbarGridRow(
            slotCount = 4,
            content = listOf(
                {
                    UniformToolbarButton(
                        text = "向左旋转",
                        onClick = { state.rotateWorkload(3) },
                        enabled = !state.busy && currentSheet != null && state.workingBitmap != null,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "向右旋转",
                        onClick = { state.rotateWorkload(1) },
                        enabled = !state.busy && currentSheet != null && state.workingBitmap != null,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "新增一行",
                        onClick = { showAddRowDialog = true },
                        enabled = !state.busy,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "删除本行",
                        onClick = { if (hasSelectedRow) showDeleteRowConfirm = true },
                        enabled = !state.busy && hasSelectedRow,
                        danger = true,
                        modifier = Modifier.weight(1f),
                    )
                },
            ),
        )

        if (state.cornerEditMode) {
            ToolbarGridRow(
                slotCount = 4,
                content = listOf(
                    {
                        UniformToolbarButton(
                            text = "按框重算",
                            onClick = { state.applyAdjustedCorners() },
                            enabled = !state.busy,
                            primary = true,
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = "取消微调",
                            onClick = { state.toggleCornerEditMode() },
                            enabled = !state.busy && currentSheet != null && state.workingBitmap != null,
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = "下一待核",
                            onClick = { state.selectNextPendingReview() },
                            enabled = currentSheet != null && currentSheet.rows.isNotEmpty(),
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = if (showWeekDetail) "汇总表格" else "周明细表",
                            onClick = { showWeekDetail = !showWeekDetail },
                            enabled = currentSheet != null,
                            modifier = Modifier.weight(1f),
                        )
                    },
                ),
            )
        } else {
            val dateLabel = String.format(Locale.US, "%04d.%02d", state.year, state.month)
            ToolbarGridRow(
                slotCount = 4,
                content = listOf(
                    {
                        UniformToolbarButton(
                            text = dateLabel,
                            onClick = { showYearMonthDialog = true },
                            enabled = !state.busy,
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = "四角微调",
                            onClick = { state.toggleCornerEditMode() },
                            enabled = !state.busy && currentSheet != null && state.workingBitmap != null,
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = "下一待核",
                            onClick = { state.selectNextPendingReview() },
                            enabled = currentSheet != null && currentSheet.rows.isNotEmpty(),
                            modifier = Modifier.weight(1f),
                        )
                    },
                    {
                        UniformToolbarButton(
                            text = if (showWeekDetail) "汇总表格" else "周明细表",
                            onClick = { showWeekDetail = !showWeekDetail },
                            enabled = currentSheet != null,
                            modifier = Modifier.weight(1f),
                        )
                    },
                ),
            )
        }

        // Warning bar when low-confidence cells still need review
        val pendingCount = currentSummary?.pendingReviewCount ?: 0
        if (currentSheet != null && pendingCount > 0) {
            val isDarkWarn = isSystemInDarkTheme()
            val warnBg = if (isDarkWarn) Color(0xFF4A2E14) else Color(0xFFFFF3CD)
            val warnFg = if (isDarkWarn) Color(0xFFFFCA62) else Color(0xFF9A3412)
            Surface(
                onClick = { state.selectNextPendingReview() },
                shape = RoundedCornerShape(10.dp),
                color = warnBg,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = "共有 $pendingCount 处低置信待确认，点击此处跳转核对",
                    style = MiuixTheme.textStyles.footnote1,
                    fontWeight = FontWeight.SemiBold,
                    color = warnFg,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                )
            }
        }

        // Photo preview & auto-zoom cell highlight canvas
        val bmp = state.workingBitmap
        if (bmp != null && currentSheet != null) {
            Card(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(200.dp),
                insideMargin = PaddingValues(0.dp),
            ) {
                InteractiveWorkloadCanvas(
                    state = state,
                    bitmap = bmp,
                )
            }
        }

        // Enlarged cell crop preview + uniform 2x5 quick-fill bar
        if (currentSheet != null && selRow in currentSheet.rows.indices) {
            val rowObj = currentSheet.rows[selRow]
            val titleLabel = if (selCol < 0) {
                "第${selRow + 1}行 姓名: ${rowObj.staffName}"
            } else if (selCol in rowObj.cells.indices) {
                val cell = rowObj.cells[selCol]
                val sName = currentSheet.columnShiftNames.getOrNull(cell.shiftCol) ?: "列${cell.shiftCol + 1}"
                val valStr = if (cell.isBlank) "空白" else formatNumberTrim(cell.value)
                "${rowObj.staffName} · 第${cell.week + 1}周 $sName = $valStr"
            } else {
                ""
            }

            Card(
                colors = CardDefaults.defaultColors(
                    color = MiuixTheme.colorScheme.surfaceContainerHigh,
                    contentColor = MiuixTheme.colorScheme.onSurfaceContainer,
                ),
                modifier = Modifier.fillMaxWidth(),
                insideMargin = PaddingValues(10.dp),
            ) {
                Column(
                    verticalArrangement = Arrangement.spacedBy(8.dp),
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
                                contentDescription = "局部放大截图",
                                contentScale = ContentScale.Fit,
                                filterQuality = FilterQuality.High,
                                modifier = Modifier
                                    .width(if (selCol < 0) 168.dp else 136.dp)
                                    .height(64.dp)
                                    .clip(RoundedCornerShape(8.dp))
                                    .background(Color.White)
                                    .border(1.dp, MiuixTheme.colorScheme.outline, RoundedCornerShape(8.dp)),
                            )
                        }
                        Column(
                            modifier = Modifier.weight(1f),
                            verticalArrangement = Arrangement.spacedBy(2.dp),
                        ) {
                            Text(
                                text = titleLabel,
                                style = MiuixTheme.textStyles.body2,
                                fontWeight = FontWeight.Bold,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis,
                            )
                            if (selCol >= 0 && selCol in rowObj.cells.indices && rowObj.cells[selCol].reviewReason.isNotBlank()) {
                                Text(
                                    text = rowObj.cells[selCol].reviewReason,
                                    style = MiuixTheme.textStyles.footnote1,
                                    color = MiuixTheme.colorScheme.error,
                                    maxLines = 2,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            } else if (selCol < 0 && rowObj.nameNeedsReview) {
                                Text(
                                    text = "姓名低置信度，请核对",
                                    style = MiuixTheme.textStyles.footnote1,
                                    color = MiuixTheme.colorScheme.error,
                                    maxLines = 1,
                                )
                            }
                        }
                    }

                    if (selCol >= 0) {
                        ToolbarGridRow(
                            slotCount = 5,
                            content = listOf(
                                {
                                    UniformQuickButton(
                                        text = "空白",
                                        onClick = {
                                            state.editCell(selRow, selCol, true, 0.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "1",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 1.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "2",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 2.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "3",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 3.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "4",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 4.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                            ),
                        )
                        ToolbarGridRow(
                            slotCount = 5,
                            content = listOf(
                                {
                                    UniformQuickButton(
                                        text = "5",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 5.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "6",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 6.0, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "0.5",
                                        onClick = {
                                            state.editCell(selRow, selCol, false, 0.5, null, advanceAfterEdit = true)
                                        },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "手输",
                                        onClick = { showCustomInputDialog = true },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "确认",
                                        primary = true,
                                        onClick = { state.confirmAndAdvance() },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                            ),
                        )
                    } else {
                        ToolbarGridRow(
                            slotCount = 2,
                            content = listOf(
                                {
                                    UniformQuickButton(
                                        text = "修改姓名",
                                        onClick = { showCustomInputDialog = true },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                                {
                                    UniformQuickButton(
                                        text = "确认",
                                        primary = true,
                                        onClick = { state.confirmAndAdvance() },
                                        modifier = Modifier.weight(1f),
                                    )
                                },
                            ),
                        )
                    }
                }
            }
        }

        // Summary or weekly grid card
        Card(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            insideMargin = PaddingValues(0.dp),
        ) {
            if (currentSheet == null || currentSummary == null) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(18.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = "请先拍照或从相册选择整月工作量表照片，也可点击“导入名单”或“新增一行”手动建表。",
                        style = MiuixTheme.textStyles.body2,
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            } else if (!showWeekDetail) {
                SummaryTableSection(
                    state = state,
                    sheet = currentSheet,
                    summary = currentSummary,
                    onSelectOrEditRow = { rowIdx ->
                        val rowCells = currentSheet.rows.getOrNull(rowIdx)?.cells.orEmpty()
                        val pendingIndices = rowCells.indices.filter { rowCells[it].status == 1 }
                        if (pendingIndices.isNotEmpty() && state.selectedRow != rowIdx) {
                            state.selectCell(rowIdx, pendingIndices.first())
                        } else if (pendingIndices.isNotEmpty() && state.selectedRow == rowIdx && state.selectedColIndex >= 0) {
                            val nextInRow = pendingIndices.firstOrNull { it > state.selectedColIndex }
                            if (nextInRow != null) {
                                state.selectCell(rowIdx, nextInRow)
                            } else {
                                state.selectCell(rowIdx, -1)
                            }
                        } else if (state.selectedRow == rowIdx && state.selectedColIndex == -1) {
                            showCustomInputDialog = true
                        } else {
                            state.selectCell(rowIdx, -1)
                        }
                    },
                )
            } else {
                WeeklyDetailSection(
                    state = state,
                    sheet = currentSheet,
                    onSelectOrEditName = { rowIdx ->
                        if (state.selectedRow == rowIdx && state.selectedColIndex == -1) {
                            showCustomInputDialog = true
                        } else {
                            state.selectCell(rowIdx, -1)
                        }
                    },
                )
            }
        }
    }

    // All 4 SuperDialogs hoisted to top-level scope so state changes never tear down active dialogs mid-dismiss.
    val activeRowObj = currentSheet?.rows?.getOrNull(selRow)
    val isNameEdit = selCol < 0
    val initInputVal = if (activeRowObj == null) {
        ""
    } else if (isNameEdit) {
        activeRowObj.staffName
    } else if (selCol in activeRowObj.cells.indices && !activeRowObj.cells[selCol].isBlank) {
        formatNumberTrim(activeRowObj.cells[selCol].value)
    } else {
        ""
    }
    var customInputText by remember { mutableStateOf(initInputVal) }
    LaunchedEffect(showCustomInputDialog, selRow, selCol) {
        if (showCustomInputDialog) {
            customInputText = initInputVal
        }
    }

    SuperDialog(
        show = showCustomInputDialog && activeRowObj != null,
        title = if (isNameEdit) "修改姓名" else "手动输入数值",
        summary = if (isNameEdit) "第 ${selRow + 1} 行姓名" else "支持 0~9、两位数或小数（如 1.5），留空保存为空白",
        onDismissRequest = { showCustomInputDialog = false },
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = customInputText,
                onValueChange = { customInputText = it },
                label = if (isNameEdit) "姓名" else "数值",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(
                    keyboardType = if (isNameEdit) KeyboardType.Text else KeyboardType.Decimal,
                ),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            if (!isNameEdit) {
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    listOf("7", "8", "9", "0.5", "1.5", "2.5").forEach { preset ->
                        Button(
                            onClick = { customInputText = preset },
                            insideMargin = PaddingValues(horizontal = 10.dp, vertical = 6.dp),
                            minWidth = 42.dp,
                            minHeight = 32.dp,
                        ) {
                            Text(preset, style = MiuixTheme.textStyles.body2, maxLines = 1)
                        }
                    }
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                UniformDialogButton(
                    text = "取消",
                    onClick = { showCustomInputDialog = false },
                    modifier = Modifier.weight(1f),
                )
                UniformDialogButton(
                    text = "保存",
                    primary = true,
                    enabled = activeRowObj != null && if (isNameEdit) {
                        customInputText.isNotBlank()
                    } else {
                        customInputText.trim().isEmpty() || customInputText.trim().toDoubleOrNull()?.let { it >= 0.0 } == true
                    },
                    onClick = {
                        if (activeRowObj != null) {
                            if (isNameEdit) {
                                if (customInputText.isNotBlank()) {
                                    state.editCell(selRow, -1, false, 0.0, customInputText.trim(), advanceAfterEdit = false)
                                }
                                showCustomInputDialog = false
                            } else {
                                val trimmed = customInputText.trim()
                                if (trimmed.isEmpty()) {
                                    state.editCell(selRow, selCol, true, 0.0, null, advanceAfterEdit = true)
                                    showCustomInputDialog = false
                                } else {
                                    trimmed.toDoubleOrNull()?.let { v ->
                                        state.editCell(selRow, selCol, false, v, null, advanceAfterEdit = true)
                                        showCustomInputDialog = false
                                    }
                                }
                            }
                        }
                    },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }

    SuperDialog(
        show = showDeleteRowConfirm && activeRowObj != null,
        title = "删除当前行",
        summary = "确定要删除第 ${selRow + 1} 行（${activeRowObj?.staffName.orEmpty()}）吗？删除后将重新编号并汇总计算。",
        onDismissRequest = { showDeleteRowConfirm = false },
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            UniformDialogButton(
                text = "取消",
                onClick = { showDeleteRowConfirm = false },
                modifier = Modifier.weight(1f),
            )
            UniformDialogButton(
                text = "确认删除",
                danger = true,
                onClick = {
                    if (activeRowObj != null) {
                        state.deleteWorkloadRow(selRow)
                    }
                    showDeleteRowConfirm = false
                },
                modifier = Modifier.weight(1f),
            )
        }
    }

    var newRowName by remember { mutableStateOf("") }
    LaunchedEffect(showAddRowDialog) {
        if (showAddRowDialog) {
            newRowName = ""
        }
    }
    val existingNames = remember(currentSheet) {
        currentSheet?.rows?.map { it.staffName }?.toSet() ?: emptySet()
    }
    val candidateStaff = remember(state.staffList.toList(), existingNames) {
        state.staffList.map { it.name }.filter { it !in existingNames }.take(8)
    }

    SuperDialog(
        show = showAddRowDialog,
        title = "新增工作量行",
        summary = "在表格末尾追加一名人员（初始单元格为空白）",
        onDismissRequest = { showAddRowDialog = false },
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = newRowName,
                onValueChange = { newRowName = it },
                label = "人员姓名",
                insideMargin = DpSize(10.dp, 12.dp),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            if (candidateStaff.isNotEmpty()) {
                Text(
                    text = "快速选择名单中未录入人员：",
                    style = MiuixTheme.textStyles.footnote1,
                    color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                )
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    candidateStaff.forEach { candidate ->
                        Button(
                            onClick = { newRowName = candidate },
                            insideMargin = PaddingValues(horizontal = 10.dp, vertical = 6.dp),
                            minHeight = 32.dp,
                        ) {
                            Text(candidate, style = MiuixTheme.textStyles.footnote1, maxLines = 1)
                        }
                    }
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                UniformDialogButton(
                    text = "取消",
                    onClick = { showAddRowDialog = false },
                    modifier = Modifier.weight(1f),
                )
                UniformDialogButton(
                    text = "确定",
                    primary = true,
                    enabled = newRowName.isNotBlank(),
                    onClick = {
                        state.addWorkloadRow(newRowName)
                        showAddRowDialog = false
                    },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }

    var editYearStr by remember { mutableStateOf(state.year.toString()) }
    var editMonthStr by remember { mutableStateOf(state.month.toString()) }
    LaunchedEffect(showYearMonthDialog) {
        if (showYearMonthDialog) {
            editYearStr = state.year.toString()
            editMonthStr = state.month.toString()
        }
    }

    SuperDialog(
        show = showYearMonthDialog,
        title = "设置统计年月",
        summary = "识别照片时会自动更新此处年月，也可手动调整",
        onDismissRequest = { showYearMonthDialog = false },
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = editYearStr,
                onValueChange = { editYearStr = it },
                label = "年份（2000~2099）",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextField(
                value = editMonthStr,
                onValueChange = { editMonthStr = it },
                label = "月份（1~12）",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                UniformDialogButton(
                    text = "取消",
                    onClick = { showYearMonthDialog = false },
                    modifier = Modifier.weight(1f),
                )
                UniformDialogButton(
                    text = "确定",
                    primary = true,
                    enabled = (editYearStr.trim().toIntOrNull() in 2000..2099) &&
                        (editMonthStr.trim().toIntOrNull() in 1..12),
                    onClick = {
                        editYearStr.trim().toIntOrNull()?.let { y -> if (y in 2000..2099) state.year = y }
                        editMonthStr.trim().toIntOrNull()?.let { m -> if (m in 1..12) state.month = m }
                        showYearMonthDialog = false
                    },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }

    SuperDialog(
        show = showExportConfirmDialog,
        title = "还有待确认项未核对",
        summary = "当前还有 ${currentSummary?.pendingReviewCount ?: 0} 处低置信识别结果未确认，建议先核对再导出。",
        onDismissRequest = { showExportConfirmDialog = false },
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            UniformDialogButton(
                text = "仍然导出",
                onClick = {
                    showExportConfirmDialog = false
                    state.exportAndShareXlsx()
                },
                modifier = Modifier.weight(1f),
            )
            UniformDialogButton(
                text = "先去核对",
                primary = true,
                onClick = {
                    showExportConfirmDialog = false
                    state.selectNextPendingReview()
                },
                modifier = Modifier.weight(1f),
            )
        }
    }
}

@Composable
private fun InteractiveWorkloadCanvas(
    state: AppState,
    bitmap: android.graphics.Bitmap,
) {
    val sheet = state.sheet ?: return
    val latestSheet by rememberUpdatedState(sheet)
    val srcW = maxOf(1, sheet.imgWidth.takeIf { it > 0 } ?: bitmap.width)
    val srcH = maxOf(1, sheet.imgHeight.takeIf { it > 0 } ?: bitmap.height)
    val imgBitmap = remember(bitmap) { bitmap.asImageBitmap() }
    val coroutineScope = rememberCoroutineScope()

    var canvasSize by remember { mutableStateOf(IntSize(1, 1)) }
    var scale by remember { mutableFloatStateOf(1f) }
    var panX by remember { mutableFloatStateOf(0f) }
    var panY by remember { mutableFloatStateOf(0f) }
    var dragCornerIdx by remember { mutableIntStateOf(-1) }

    val fitTransform: () -> Triple<Float, Float, Float> = {
        val sx = canvasSize.width.toFloat() / srcW.toFloat()
        val sy = canvasSize.height.toFloat() / srcH.toFloat()
        val fs = min(sx, sy).coerceAtLeast(0.05f)
        val px = (canvasSize.width - srcW * fs) * 0.5f
        val py = (canvasSize.height - srcH * fs) * 0.5f
        Triple(fs, px, py)
    }

    LaunchedEffect(bitmap, canvasSize, state.cornerEditMode) {
        if (canvasSize.width > 1 && canvasSize.height > 1) {
            if (state.cornerEditMode || state.selectedRow !in sheet.rows.indices) {
                val (fs, px, py) = fitTransform()
                scale = fs
                panX = px
                panY = py
            }
        }
    }

    LaunchedEffect(state.selectedRow, state.selectedColIndex, canvasSize, state.cornerEditMode) {
        if (canvasSize.width <= 1 || canvasSize.height <= 1 || state.cornerEditMode) return@LaunchedEffect
        val r = state.selectedRow
        val c = state.selectedColIndex
        val rowObj = latestSheet.rows.getOrNull(r) ?: return@LaunchedEffect
        val quad = if (c < 0) rowObj.nameQuad else rowObj.cells.getOrNull(c)?.quad ?: return@LaunchedEffect
        if (quad.size != 4) return@LaunchedEffect

        val minX = quad.minOf { it.x }
        val maxX = quad.maxOf { it.x }
        val minY = quad.minOf { it.y }
        val maxY = quad.maxOf { it.y }
        val qw = maxX - minX
        val qh = maxY - minY
        if (qw <= 1f || qh <= 1f) return@LaunchedEffect

        val cx = (minX + maxX) * 0.5f
        val cy = (minY + maxY) * 0.5f
        val cw = canvasSize.width.toFloat()
        val ch = canvasSize.height.toFloat()
        val fitScale = min(cw / srcW.toFloat(), ch / srcH.toFloat()).coerceAtLeast(0.05f)

        val contextScale = min(cw / (qw * 5.2f), ch / (qh * 4.0f))
        val targetScale = maxOf(fitScale * 3.2f, contextScale)
            .coerceIn(fitScale * 2.4f, maxOf(6.0f, fitScale * 10.0f))

        var targetPanX = cw * 0.5f - cx * targetScale
        var targetPanY = ch * 0.5f - cy * targetScale
        val scaledW = srcW * targetScale
        val scaledH = srcH * targetScale
        targetPanX = if (scaledW >= cw) targetPanX.coerceIn(cw - scaledW, 0f) else (cw - scaledW) * 0.5f
        targetPanY = if (scaledH >= ch) targetPanY.coerceIn(ch - scaledH, 0f) else (ch - scaledH) * 0.5f

        val startScale = scale
        val startPanX = panX
        val startPanY = panY
        val anim = Animatable(0f)
        anim.animateTo(
            targetValue = 1f,
            animationSpec = tween(durationMillis = 260, easing = FastOutSlowInEasing),
        ) {
            val t = value
            scale = startScale + (targetScale - startScale) * t
            panX = startPanX + (targetPanX - startPanX) * t
            panY = startPanY + (targetPanY - startPanY) * t
        }
    }

    val modifier = Modifier
        .fillMaxSize()
        .clipToBounds()
        .background(Color(0xFF1E1E22))
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
                    .pointerInput("tapCell") {
                        detectTapGestures { offset ->
                            val curSheet = latestSheet
                            val ix = (offset.x - panX) / scale
                            val iy = (offset.y - panY) / scale
                            for (rIdx in curSheet.rows.indices) {
                                val row = curSheet.rows[rIdx]
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
                            val minScale = minOf(
                                0.15f,
                                min(
                                    canvasSize.width.toFloat() / srcW.toFloat(),
                                    canvasSize.height.toFloat() / srcH.toFloat(),
                                ) * 0.8f,
                            )
                            val maxScale = maxOf(8.0f, minScale * 24f)
                            val oldScale = scale
                            val newScale = (scale * zoom).coerceIn(minScale, maxScale)
                            val factor = newScale / oldScale
                            panX = (panX - centroid.x) * factor + centroid.x + pan.x
                            panY = (panY - centroid.y) * factor + centroid.y + pan.y
                            scale = newScale
                        }
                    }
            },
        )

    Box(modifier = Modifier.fillMaxSize()) {
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

                for (rIdx in sheet.rows.indices) {
                    val row = sheet.rows[rIdx]
                    if (row.nameQuad.size == 4) {
                        val isSel = (rIdx == state.selectedRow && state.selectedColIndex == -1)
                        if (isSel || row.nameNeedsReview) {
                            val path = quadToPath(row.nameQuad)
                            drawPath(
                                path = path,
                                color = if (isSel) Color(0xFF3482FF) else Color(0xFFFF9800),
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
                                color = if (isSel) Color(0xFF3482FF) else Color(0xFFFF9800),
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

        if (!state.cornerEditMode && canvasSize.width > 1 && canvasSize.height > 1) {
            Surface(
                onClick = {
                    val (targetScale, targetPanX, targetPanY) = fitTransform()
                    val startScale = scale
                    val startPanX = panX
                    val startPanY = panY
                    coroutineScope.launch {
                        val anim = Animatable(0f)
                        anim.animateTo(1f, tween(200, easing = FastOutSlowInEasing)) {
                            scale = startScale + (targetScale - startScale) * value
                            panX = startPanX + (targetPanX - startPanX) * value
                            panY = startPanY + (targetPanY - startPanY) * value
                        }
                    }
                },
                shape = RoundedCornerShape(8.dp),
                color = Color.Black.copy(alpha = 0.55f),
                contentColor = Color.White,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(8.dp),
            ) {
                Text(
                    text = "全景",
                    style = MiuixTheme.textStyles.footnote1,
                    color = Color.White,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun SummaryTableSection(
    state: AppState,
    sheet: WorkloadSheetUi,
    summary: com.scluzlep.nurseocreport.SheetSummaryUi,
    onSelectOrEditRow: (Int) -> Unit,
) {
    val isDark = isSystemInDarkTheme()
    val headerBg = if (isDark) Color(0xFF1F3247) else Color(0xFFEAF2FF)
    val selectedBg = if (isDark) Color(0xFF1E3A5F) else Color(0xFFD6E6FF)
    val reviewBg = if (isDark) Color(0xFF473619) else Color(0xFFFFF4DC)
    val zebraBg = if (isDark) Color(0xFF1C1C1E) else Color(0xFFF8F9FB)
    val totalBg = if (isDark) Color(0xFF2C2C2E) else Color(0xFFECEEF2)

    val hScroll = rememberScrollState()
    val enabledRules = state.shiftRules
    Column(modifier = Modifier.fillMaxSize().horizontalScroll(hScroll)) {
        Row(
            modifier = Modifier
                .background(headerBg)
                .padding(horizontal = 10.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("姓名", modifier = Modifier.width(96.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
            enabledRules.forEach { r ->
                Text(r.name, modifier = Modifier.width(68.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text("加权合计", modifier = Modifier.width(76.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
            Text("系数", modifier = Modifier.width(56.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
            Text("最终工作量", modifier = Modifier.width(88.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f)) {
            itemsIndexed(summary.people) { idx, person ->
                val rowObj = sheet.rows.getOrNull(idx)
                val needsRev = rowObj?.nameNeedsReview == true
                val rowDigitPending = rowObj?.cells?.count { it.status == 1 } ?: 0
                val hasAnyWarning = needsRev || rowDigitPending > 0 || !person.matchedInStaff
                val warnColor = if (isDark) Color(0xFFFFB74D) else Color(0xFFB45309)
                Row(
                    modifier = Modifier
                        .background(
                            when {
                                idx == state.selectedRow -> selectedBg
                                hasAnyWarning -> reviewBg
                                idx % 2 == 1 -> zebraBg
                                else -> MiuixTheme.colorScheme.surfaceContainer
                            }
                        )
                        .clickable { onSelectOrEditRow(idx) }
                        .padding(horizontal = 10.dp, vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.width(96.dp)) {
                        Text(
                            text = person.name,
                            style = MiuixTheme.textStyles.body2,
                            fontWeight = FontWeight.Medium,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        if (rowDigitPending > 0) {
                            Text(
                                text = "${rowDigitPending}处待核",
                                style = MiuixTheme.textStyles.footnote1,
                                fontWeight = FontWeight.SemiBold,
                                color = warnColor,
                                maxLines = 1,
                            )
                        } else if (needsRev) {
                            Text(
                                text = "姓名待核",
                                style = MiuixTheme.textStyles.footnote1,
                                color = MiuixTheme.colorScheme.error,
                                maxLines = 1,
                            )
                        }
                    }
                    enabledRules.indices.forEach { rIdx ->
                        val v = person.shiftTotals.getOrElse(rIdx) { 0.0 }
                        Text(formatNumberTrim(v), modifier = Modifier.width(68.dp), style = MiuixTheme.textStyles.body2, maxLines = 1)
                    }
                    Text(formatNumber2dp(person.weightedSum), modifier = Modifier.width(76.dp), style = MiuixTheme.textStyles.body2, maxLines = 1)
                    Text(formatNumber2dp(person.effectiveCoeff), modifier = Modifier.width(56.dp), style = MiuixTheme.textStyles.body2, maxLines = 1)
                    Text(
                        text = formatNumber2dp(person.finalScore),
                        modifier = Modifier.width(88.dp),
                        style = MiuixTheme.textStyles.body2,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                    )
                }
                HorizontalDivider()
            }
            item {
                Row(
                    modifier = Modifier
                        .background(totalBg)
                        .padding(horizontal = 10.dp, vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("合计", modifier = Modifier.width(96.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
                    enabledRules.indices.forEach { rIdx ->
                        val gt = summary.shiftGrandTotals.getOrElse(rIdx) { 0.0 }
                        Text(formatNumberTrim(gt), modifier = Modifier.width(68.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
                    }
                    Text(formatNumber2dp(summary.weightedGrandTotal), modifier = Modifier.width(76.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
                    Text("-", modifier = Modifier.width(56.dp), style = MiuixTheme.textStyles.body2)
                    Text(formatNumber2dp(summary.finalGrandTotal), modifier = Modifier.width(88.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
                }
            }
        }
    }
}

@Composable
private fun WeeklyDetailSection(
    state: AppState,
    sheet: WorkloadSheetUi,
    onSelectOrEditName: (Int) -> Unit,
) {
    val isDark = isSystemInDarkTheme()
    val headerBg = if (isDark) Color(0xFF1F3247) else Color(0xFFEAF2FF)
    val selectedBg = if (isDark) Color(0xFF1E3A5F) else Color(0xFFCCE0FF)
    val reviewBg = if (isDark) Color(0xFF473619) else Color(0xFFFFF4DC)
    val confirmedBg = if (isDark) Color(0xFF1C3824) else Color(0xFFE8F5E9)

    val hScroll = rememberScrollState()
    val colsPerWeek = sheet.columnShiftNames.size
    Column(modifier = Modifier.fillMaxSize().horizontalScroll(hScroll)) {
        Row(
            modifier = Modifier
                .background(headerBg)
                .padding(horizontal = 10.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("姓名", modifier = Modifier.width(80.dp), style = MiuixTheme.textStyles.body2, fontWeight = FontWeight.Bold, maxLines = 1)
            for (w in 0 until sheet.weeks) {
                for (sc in 0 until colsPerWeek) {
                    Text(
                        text = "W${w + 1}${sheet.columnShiftNames[sc]}",
                        modifier = Modifier.width(62.dp),
                        style = MiuixTheme.textStyles.footnote1,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f)) {
            itemsIndexed(sheet.rows) { rIdx, row ->
                Row(
                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = row.staffName,
                        modifier = Modifier
                            .width(80.dp)
                            .clickable { onSelectOrEditName(rIdx) },
                        style = MiuixTheme.textStyles.body2,
                        fontWeight = FontWeight.Medium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    row.cells.forEachIndexed { cIdx, cell ->
                        val isSel = (rIdx == state.selectedRow && cIdx == state.selectedColIndex)
                        val bg = when {
                            isSel -> selectedBg
                            cell.status == 1 -> reviewBg
                            cell.status == 2 -> confirmedBg
                            else -> Color.Transparent
                        }
                        Box(
                            modifier = Modifier
                                .width(62.dp)
                                .clip(RoundedCornerShape(6.dp))
                                .background(bg)
                                .clickable { state.selectCell(rIdx, cIdx) }
                                .padding(vertical = 6.dp, horizontal = 6.dp),
                        ) {
                            val txt = if (cell.isBlank) "" else formatNumberTrim(cell.value)
                            val isReview = (cell.status == 1)
                            val displayTxt = if (isReview) "${txt.ifEmpty { "空" }}?" else txt.ifEmpty { "-" }
                            if (isReview) {
                                Text(
                                    text = displayTxt,
                                    style = MiuixTheme.textStyles.body2,
                                    fontWeight = FontWeight.Bold,
                                    color = if (isDark) Color(0xFFFFB74D) else Color(0xFFB45309),
                                    maxLines = 1,
                                )
                            } else {
                                Text(
                                    text = displayTxt,
                                    style = MiuixTheme.textStyles.body2,
                                    maxLines = 1,
                                )
                            }
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
