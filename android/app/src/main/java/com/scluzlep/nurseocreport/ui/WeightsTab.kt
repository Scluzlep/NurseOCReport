package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.scluzlep.nurseocreport.AppState
import com.scluzlep.nurseocreport.ShiftRuleUi
import com.scluzlep.nurseocreport.formatNumberTrim

@Composable
fun WeightsTab(state: AppState) {
    var showAddDialog by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("人员系数参与开关", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("启用职称系数")
                    Switch(
                        checked = state.enableTitleCoeff,
                        onCheckedChange = { state.setTitleCoeffEnabled(it) },
                    )
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("启用职务系数")
                    Switch(
                        checked = state.enableDutyCoeff,
                        onCheckedChange = { state.setDutyCoeffEnabled(it) },
                    )
                }
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(onClick = { showAddDialog = true }) {
                Text("新增班次")
            }
            OutlinedButton(onClick = { state.resetDefaultShiftRules() }) {
                Text("恢复默认")
            }
        }

        Card(modifier = Modifier.weight(1f).fillMaxWidth()) {
            Column(modifier = Modifier.fillMaxSize()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("启用", modifier = Modifier.width(48.dp), fontWeight = FontWeight.Bold)
                    Text("班次名称", modifier = Modifier.weight(1.2f), fontWeight = FontWeight.Bold)
                    Text("权重系数", modifier = Modifier.weight(1f), fontWeight = FontWeight.Bold)
                    Text("操作", modifier = Modifier.width(56.dp), fontWeight = FontWeight.Bold)
                }
                HorizontalDivider()
                LazyColumn(modifier = Modifier.fillMaxSize()) {
                    itemsIndexed(state.shiftRules, key = { idx, item -> "${idx}_${item.code}_${item.name}" }) { idx, rule ->
                        ShiftRuleRowItem(
                            rule = rule,
                            onChange = { state.updateShiftRule(idx, it) },
                            onDelete = { state.removeShiftRule(idx) },
                        )
                        HorizontalDivider()
                    }
                }
            }
        }
    }

    if (showAddDialog) {
        var newName by remember { mutableStateOf("") }
        var newWeight by remember { mutableStateOf("1.0") }
        AlertDialog(
            onDismissRequest = { showAddDialog = false },
            title = { Text("新增班次权重") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = newName,
                        onValueChange = { newName = it },
                        label = { Text("班次名称") },
                        singleLine = true,
                    )
                    OutlinedTextField(
                        value = newWeight,
                        onValueChange = { newWeight = it },
                        label = { Text("权重系数") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        singleLine = true,
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val w = newWeight.toDoubleOrNull() ?: 1.0
                        state.addShiftRule(newName, w)
                        showAddDialog = false
                    }
                ) {
                    Text("确定")
                }
            },
            dismissButton = {
                TextButton(onClick = { showAddDialog = false }) {
                    Text("取消")
                }
            },
        )
    }
}

@Composable
private fun ShiftRuleRowItem(
    rule: ShiftRuleUi,
    onChange: (ShiftRuleUi) -> Unit,
    onDelete: () -> Unit,
) {
    var nameText by remember(rule.name) { mutableStateOf(rule.name) }
    var weightText by remember(rule.weight) { mutableStateOf(formatNumberTrim(rule.weight)) }

    LaunchedEffect(rule.name) {
        if (nameText != rule.name) nameText = rule.name
    }
    LaunchedEffect(rule.weight) {
        val parsed = weightText.toDoubleOrNull()
        if (parsed == null || kotlin.math.abs(parsed - rule.weight) > 1e-6) {
            weightText = formatNumberTrim(rule.weight)
        }
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(
            checked = rule.enabled,
            onCheckedChange = { checked -> onChange(rule.copy(enabled = checked)) },
            modifier = Modifier.width(48.dp),
        )
        OutlinedTextField(
            value = nameText,
            onValueChange = { txt ->
                nameText = txt
                if (txt.isNotBlank()) {
                    onChange(rule.copy(name = txt.trim()))
                }
            },
            singleLine = true,
            modifier = Modifier.weight(1.2f).padding(end = 6.dp),
        )
        Spacer(modifier = Modifier.width(4.dp))
        OutlinedTextField(
            value = weightText,
            onValueChange = { txt ->
                weightText = txt
                val v = txt.toDoubleOrNull()
                if (v != null && v >= 0.0) {
                    onChange(rule.copy(weight = v))
                }
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            singleLine = true,
            modifier = Modifier.weight(1f),
        )
        TextButton(
            onClick = onDelete,
            modifier = Modifier.width(56.dp),
        ) {
            Text("删")
        }
    }
    Spacer(modifier = Modifier.height(2.dp))
}
