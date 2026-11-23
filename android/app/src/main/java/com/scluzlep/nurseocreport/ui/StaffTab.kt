package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import com.scluzlep.nurseocreport.AppState
import com.scluzlep.nurseocreport.StaffEntryUi
import com.scluzlep.nurseocreport.formatNumber2dp
import com.scluzlep.nurseocreport.formatNumberTrim
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.ButtonDefaults
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextButton
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.extra.SuperDialog
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.utils.PressFeedbackType

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun StaffTab(
    state: AppState,
    onTakeCoeffPhoto: () -> Unit,
    onPickCoeffPhoto: () -> Unit,
    onImportNames: () -> Unit,
) {
    var showAddDialog by remember { mutableStateOf(false) }
    var editingIndex by remember { mutableIntStateOf(-1) }
    var showClearConfirm by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        FlowRow(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Button(
                onClick = onTakeCoeffPhoto,
                enabled = !state.busy,
                colors = ButtonDefaults.buttonColorsPrimary(),
                insideMargin = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text("拍照识别", maxLines = 1)
            }
            Button(
                onClick = onPickCoeffPhoto,
                enabled = !state.busy,
                insideMargin = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text("相册选图", maxLines = 1)
            }
            Button(
                onClick = onImportNames,
                enabled = !state.busy,
                insideMargin = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text("导入名单", maxLines = 1)
            }
            Button(
                onClick = { showAddDialog = true },
                enabled = !state.busy,
                insideMargin = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text("新增人员", maxLines = 1)
            }
            Button(
                onClick = { showClearConfirm = true },
                enabled = !state.busy && state.staffList.isNotEmpty(),
                colors = ButtonDefaults.buttonColors(
                    color = MiuixTheme.colorScheme.errorContainer,
                    contentColor = MiuixTheme.colorScheme.error,
                ),
                insideMargin = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
            ) {
                Text("清空", maxLines = 1)
            }
        }

        if (state.staffList.isEmpty()) {
            Card(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                insideMargin = PaddingValues(20.dp),
            ) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = "人员系数表当前为空。请拍照或选图识别人员系数表，或点击“导入名单”/“新增人员”录入。",
                        style = MiuixTheme.textStyles.body2,
                        color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                    )
                }
            }
        } else {
            Text(
                text = "共 ${state.staffList.size} 人（点击任意行可编辑或删除）",
                style = MiuixTheme.textStyles.footnote1,
                color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            LazyColumn(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                itemsIndexed(state.staffList) { idx, entry ->
                    StaffRowCard(
                        index = idx,
                        entry = entry,
                        totalCoeff = state.computeEffectiveCoeff(entry),
                        onClick = { editingIndex = idx },
                    )
                }
            }
        }
    }

    val editingEntry = state.staffList.getOrNull(editingIndex)
    StaffEditDialog(
        show = showAddDialog || editingEntry != null,
        isEdit = editingEntry != null,
        initialEntry = editingEntry ?: StaffEntryUi("", 1.0, 0.0),
        rowNumber = if (editingEntry != null) editingIndex + 1 else 0,
        computeTotal = { title, duty ->
            state.computeEffectiveCoeff(StaffEntryUi("", title, duty))
        },
        onSave = { updated ->
            if (editingEntry != null && editingIndex in state.staffList.indices) {
                state.updateStaffEntry(editingIndex, updated)
                editingIndex = -1
            } else {
                state.addStaffEntry(updated.name, updated.titleCoeff, updated.dutyCoeff)
                showAddDialog = false
            }
        },
        onDelete = if (editingEntry != null) {
            {
                if (editingIndex in state.staffList.indices) {
                    state.removeStaffEntry(editingIndex)
                }
                editingIndex = -1
            }
        } else {
            null
        },
        onDismiss = {
            showAddDialog = false
            editingIndex = -1
        },
    )

    SuperDialog(
        show = showClearConfirm,
        title = "清空人员系数表",
        summary = "确定要清空全部 ${state.staffList.size} 名人员系数吗？",
        onDismissRequest = { showClearConfirm = false },
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextButton(
                text = "取消",
                onClick = { showClearConfirm = false },
                modifier = Modifier.weight(1f),
            )
            Button(
                onClick = {
                    state.clearStaffList()
                    showClearConfirm = false
                },
                colors = ButtonDefaults.buttonColors(
                    color = MiuixTheme.colorScheme.errorContainer,
                    contentColor = MiuixTheme.colorScheme.error,
                ),
                modifier = Modifier.weight(1f),
            ) {
                Text("确认清空", maxLines = 1)
            }
        }
    }
}

@Composable
private fun StaffRowCard(
    index: Int,
    entry: StaffEntryUi,
    totalCoeff: Double,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        insideMargin = PaddingValues(horizontal = 12.dp, vertical = 11.dp),
        pressFeedbackType = PressFeedbackType.Sink,
        showIndication = true,
        onClick = onClick,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box(
                modifier = Modifier
                    .defaultMinSize(minWidth = 26.dp)
                    .clip(RoundedCornerShape(6.dp))
                    .background(MiuixTheme.colorScheme.secondaryContainer)
                    .padding(horizontal = 6.dp, vertical = 3.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = "${index + 1}",
                    style = MiuixTheme.textStyles.footnote1,
                    color = MiuixTheme.colorScheme.onSecondaryVariant,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    softWrap = false,
                )
            }

            Text(
                text = entry.name,
                style = MiuixTheme.textStyles.body1,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                softWrap = false,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )

            StaffBadge(
                text = "职称 ${formatNumberTrim(entry.titleCoeff)}",
                bgColor = MiuixTheme.colorScheme.secondaryContainer,
                textColor = MiuixTheme.colorScheme.onSecondaryVariant,
            )
            StaffBadge(
                text = "职务 ${formatNumberTrim(entry.dutyCoeff)}",
                bgColor = MiuixTheme.colorScheme.secondaryContainer,
                textColor = MiuixTheme.colorScheme.onSecondaryVariant,
            )
            StaffBadge(
                text = "总 ${formatNumber2dp(totalCoeff)}",
                bgColor = MiuixTheme.colorScheme.tertiaryContainer,
                textColor = MiuixTheme.colorScheme.onTertiaryContainer,
                bold = true,
            )
        }
    }
}

@Composable
private fun StaffBadge(
    text: String,
    bgColor: androidx.compose.ui.graphics.Color,
    textColor: androidx.compose.ui.graphics.Color,
    bold: Boolean = false,
) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(bgColor)
            .padding(horizontal = 7.dp, vertical = 4.dp),
    ) {
        Text(
            text = text,
            style = MiuixTheme.textStyles.footnote1,
            color = textColor,
            fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun StaffEditDialog(
    show: Boolean,
    isEdit: Boolean,
    initialEntry: StaffEntryUi,
    rowNumber: Int,
    computeTotal: (Double, Double) -> Double,
    onSave: (StaffEntryUi) -> Unit,
    onDelete: (() -> Unit)?,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf(initialEntry.name) }
    var titleStr by remember { mutableStateOf(formatNumberTrim(initialEntry.titleCoeff)) }
    var dutyStr by remember { mutableStateOf(formatNumberTrim(initialEntry.dutyCoeff)) }

    LaunchedEffect(show, initialEntry) {
        if (show) {
            name = initialEntry.name
            titleStr = formatNumberTrim(initialEntry.titleCoeff)
            dutyStr = formatNumberTrim(initialEntry.dutyCoeff)
        }
    }

    val previewTitle = titleStr.toDoubleOrNull() ?: initialEntry.titleCoeff
    val previewDuty = dutyStr.toDoubleOrNull() ?: initialEntry.dutyCoeff
    val previewTotal = computeTotal(previewTitle, previewDuty)

    SuperDialog(
        show = show,
        title = if (isEdit) "编辑人员系数" else "新增人员系数",
        summary = if (isEdit) "第 $rowNumber 项 · 当前有效总系数 ${formatNumber2dp(previewTotal)}" else "填写姓名及职称/职务系数",
        onDismissRequest = onDismiss,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = name,
                onValueChange = { name = it },
                label = "姓名",
                insideMargin = DpSize(10.dp, 12.dp),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextField(
                value = titleStr,
                onValueChange = { titleStr = it },
                label = "职称系数",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextField(
                value = dutyStr,
                onValueChange = { dutyStr = it },
                label = "职务系数",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (onDelete != null) {
                    Button(
                        onClick = onDelete,
                        colors = ButtonDefaults.buttonColors(
                            color = MiuixTheme.colorScheme.errorContainer,
                            contentColor = MiuixTheme.colorScheme.error,
                        ),
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("删除", maxLines = 1)
                    }
                }
                TextButton(
                    text = "取消",
                    onClick = onDismiss,
                    modifier = Modifier.weight(1f),
                )
                TextButton(
                    text = if (isEdit) "保存" else "确定",
                    enabled = name.isNotBlank(),
                    colors = ButtonDefaults.textButtonColorsPrimary(),
                    onClick = {
                        onSave(
                            StaffEntryUi(
                                name = name.trim(),
                                titleCoeff = titleStr.toDoubleOrNull() ?: 1.0,
                                dutyCoeff = dutyStr.toDoubleOrNull() ?: 0.0,
                            )
                        )
                    },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}
