package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
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
import com.scluzlep.nurseocreport.ShiftRuleUi
import com.scluzlep.nurseocreport.formatNumberTrim
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.HorizontalDivider
import top.yukonga.miuix.kmp.basic.Switch
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.extra.SuperDialog
import top.yukonga.miuix.kmp.extra.SuperSwitch
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.utils.PressFeedbackType

@Composable
fun WeightsTab(state: AppState) {
    var showAddDialog by remember { mutableStateOf(false) }
    var editingIndex by remember { mutableIntStateOf(-1) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            insideMargin = PaddingValues(vertical = 4.dp),
        ) {
            Text(
                text = "人员系数参与开关",
                style = MiuixTheme.textStyles.subtitle,
                color = MiuixTheme.colorScheme.onSurfaceVariantSummary,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            SuperSwitch(
                checked = state.enableTitleCoeff,
                onCheckedChange = { state.setTitleCoeffEnabled(it) },
                title = "启用职称系数",
                summary = "关闭后汇总计算不再计入职称系数",
            )
            HorizontalDivider(modifier = Modifier.padding(horizontal = 16.dp))
            SuperSwitch(
                checked = state.enableDutyCoeff,
                onCheckedChange = { state.setDutyCoeffEnabled(it) },
                title = "启用职务系数",
                summary = "关闭后汇总计算不再计入职务系数",
            )
        }

        ToolbarGridRow(
            slotCount = 4,
            content = listOf(
                {
                    UniformToolbarButton(
                        text = "新增班次",
                        onClick = { showAddDialog = true },
                        primary = true,
                        modifier = Modifier.weight(1f),
                    )
                },
                {
                    UniformToolbarButton(
                        text = "恢复默认",
                        onClick = { state.resetDefaultShiftRules() },
                        modifier = Modifier.weight(1f),
                    )
                },
            ),
        )

        Text(
            text = "班次权重规则（点击卡片可修改名称、权重或删除）",
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
            itemsIndexed(
                state.shiftRules,
                key = { idx, item -> "${idx}_${item.code}_${item.name}" },
            ) { idx, rule ->
                ShiftRuleCard(
                    rule = rule,
                    onToggleEnabled = { checked ->
                        state.updateShiftRule(idx, rule.copy(enabled = checked))
                    },
                    onClick = { editingIndex = idx },
                )
            }
        }
    }

    val editingRule = state.shiftRules.getOrNull(editingIndex)
    ShiftRuleEditDialog(
        show = showAddDialog || editingRule != null,
        isEdit = editingRule != null,
        initialRule = editingRule ?: ShiftRuleUi(code = "", name = "", weight = 1.0, enabled = true),
        onSave = { updated ->
            if (editingRule != null && editingIndex in state.shiftRules.indices) {
                state.updateShiftRule(editingIndex, updated)
                editingIndex = -1
            } else {
                state.addShiftRule(updated.name, updated.weight)
                showAddDialog = false
            }
        },
        onDelete = if (editingRule != null) {
            {
                if (editingIndex in state.shiftRules.indices) {
                    state.removeShiftRule(editingIndex)
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
}

@Composable
private fun ShiftRuleCard(
    rule: ShiftRuleUi,
    onToggleEnabled: (Boolean) -> Unit,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        insideMargin = PaddingValues(horizontal = 12.dp, vertical = 10.dp),
        pressFeedbackType = PressFeedbackType.Sink,
        showIndication = true,
        onClick = onClick,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Switch(
                checked = rule.enabled,
                onCheckedChange = onToggleEnabled,
            )

            Text(
                text = rule.name,
                style = MiuixTheme.textStyles.body1,
                fontWeight = FontWeight.SemiBold,
                color = if (rule.enabled) {
                    MiuixTheme.colorScheme.onSurfaceContainer
                } else {
                    MiuixTheme.colorScheme.disabledOnSurface
                },
                maxLines = 1,
                softWrap = false,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )

            if (rule.code.isNotBlank() && rule.code != rule.name) {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(MiuixTheme.colorScheme.secondaryContainer)
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                ) {
                    Text(
                        text = rule.code,
                        style = MiuixTheme.textStyles.footnote1,
                        color = MiuixTheme.colorScheme.onSecondaryVariant,
                        maxLines = 1,
                        softWrap = false,
                    )
                }
            }

            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .background(
                        if (rule.enabled) {
                            MiuixTheme.colorScheme.tertiaryContainer
                        } else {
                            MiuixTheme.colorScheme.secondaryContainer
                        }
                    )
                    .padding(horizontal = 9.dp, vertical = 4.dp),
            ) {
                Text(
                    text = "权重 ${formatNumberTrim(rule.weight)}",
                    style = MiuixTheme.textStyles.footnote1,
                    color = if (rule.enabled) {
                        MiuixTheme.colorScheme.onTertiaryContainer
                    } else {
                        MiuixTheme.colorScheme.onSecondaryVariant
                    },
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    softWrap = false,
                )
            }
        }
    }
}

@Composable
private fun ShiftRuleEditDialog(
    show: Boolean,
    isEdit: Boolean,
    initialRule: ShiftRuleUi,
    onSave: (ShiftRuleUi) -> Unit,
    onDelete: (() -> Unit)?,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf(initialRule.name) }
    var weightStr by remember { mutableStateOf(formatNumberTrim(initialRule.weight)) }
    var enabled by remember { mutableStateOf(initialRule.enabled) }

    LaunchedEffect(show, initialRule) {
        if (show) {
            name = initialRule.name
            weightStr = formatNumberTrim(initialRule.weight)
            enabled = initialRule.enabled
        }
    }

    SuperDialog(
        show = show,
        title = if (isEdit) "编辑班次权重" else "新增班次权重",
        summary = if (isEdit && initialRule.code.isNotBlank()) "编码 ${initialRule.code}" else "设置班次名称与加权系数",
        onDismissRequest = onDismiss,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = name,
                onValueChange = { name = it },
                label = "班次名称",
                insideMargin = DpSize(10.dp, 12.dp),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextField(
                value = weightStr,
                onValueChange = { weightStr = it },
                label = "权重系数",
                insideMargin = DpSize(10.dp, 12.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            if (isEdit) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 2.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("参与合计计算", style = MiuixTheme.textStyles.body2)
                    Switch(
                        checked = enabled,
                        onCheckedChange = { enabled = it },
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (onDelete != null) {
                    UniformDialogButton(
                        text = "删除",
                        onClick = onDelete,
                        danger = true,
                        modifier = Modifier.weight(1f),
                    )
                }
                UniformDialogButton(
                    text = "取消",
                    onClick = onDismiss,
                    modifier = Modifier.weight(1f),
                )
                UniformDialogButton(
                    text = if (isEdit) "保存" else "确定",
                    primary = true,
                    enabled = name.isNotBlank() && (weightStr.toDoubleOrNull()?.let { it >= 0.0 } == true),
                    onClick = {
                        val w = weightStr.toDoubleOrNull() ?: 1.0
                        onSave(
                            initialRule.copy(
                                code = if (initialRule.code.isBlank()) name.trim() else initialRule.code,
                                name = name.trim(),
                                weight = w,
                                enabled = enabled,
                            )
                        )
                    },
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}
