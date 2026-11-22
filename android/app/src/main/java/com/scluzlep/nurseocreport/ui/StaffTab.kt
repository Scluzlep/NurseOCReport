package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import com.scluzlep.nurseocreport.StaffEntryUi
import com.scluzlep.nurseocreport.formatNumber2dp
import com.scluzlep.nurseocreport.formatNumberTrim

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun StaffTab(
    state: AppState,
    onTakeCoeffPhoto: () -> Unit,
    onPickCoeffPhoto: () -> Unit,
    onImportNames: () -> Unit,
) {
    var showAddDialog by remember { mutableStateOf(false) }

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
            Button(onClick = onTakeCoeffPhoto, enabled = !state.busy) {
                Text("拍照识别")
            }
            OutlinedButton(onClick = onPickCoeffPhoto, enabled = !state.busy) {
                Text("相册选图")
            }
            OutlinedButton(onClick = onImportNames, enabled = !state.busy) {
                Text("导入名单")
            }
            OutlinedButton(onClick = { showAddDialog = true }, enabled = !state.busy) {
                Text("新增人员")
            }
            OutlinedButton(onClick = { state.clearStaffList() }, enabled = !state.busy && state.staffList.isNotEmpty()) {
                Text("清空")
            }
        }

        Card(modifier = Modifier.weight(1f).fillMaxWidth()) {
            if (state.staffList.isEmpty()) {
                Box(
                    modifier = Modifier.fillMaxSize().padding(20.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        "人员系数表当前为空。请拍照或选图识别人员系数表，或导入纯名单文件后手动填入系数。",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            } else {
                Column(modifier = Modifier.fillMaxSize()) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 8.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("序", modifier = Modifier.width(32.dp), fontWeight = FontWeight.Bold)
                        Text("姓名", modifier = Modifier.weight(1.1f), fontWeight = FontWeight.Bold)
                        Text("职称", modifier = Modifier.weight(0.8f), fontWeight = FontWeight.Bold)
                        Text("职务", modifier = Modifier.weight(0.8f), fontWeight = FontWeight.Bold)
                        Text("总系数", modifier = Modifier.width(64.dp), fontWeight = FontWeight.Bold)
                        Text("删", modifier = Modifier.width(42.dp), fontWeight = FontWeight.Bold)
                    }
                    HorizontalDivider()
                    LazyColumn(modifier = Modifier.fillMaxSize()) {
                        itemsIndexed(state.staffList) { idx, entry ->
                            StaffRowItem(
                                index = idx,
                                entry = entry,
                                totalCoeff = state.computeEffectiveCoeff(entry),
                                onChange = { state.updateStaffEntry(idx, it) },
                                onDelete = { state.removeStaffEntry(idx) },
                            )
                            HorizontalDivider()
                        }
                    }
                }
            }
        }
    }

    if (showAddDialog) {
        var name by remember { mutableStateOf("") }
        var titleStr by remember { mutableStateOf("1.0") }
        var dutyStr by remember { mutableStateOf("0.0") }
        AlertDialog(
            onDismissRequest = { showAddDialog = false },
            title = { Text("新增人员系数") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = name,
                        onValueChange = { name = it },
                        label = { Text("姓名") },
                        singleLine = true,
                    )
                    OutlinedTextField(
                        value = titleStr,
                        onValueChange = { titleStr = it },
                        label = { Text("职称系数") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        singleLine = true,
                    )
                    OutlinedTextField(
                        value = dutyStr,
                        onValueChange = { dutyStr = it },
                        label = { Text("职务系数") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        singleLine = true,
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        state.addStaffEntry(
                            name = name,
                            titleCoeff = titleStr.toDoubleOrNull() ?: 1.0,
                            dutyCoeff = dutyStr.toDoubleOrNull() ?: 0.0,
                        )
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
private fun StaffRowItem(
    index: Int,
    entry: StaffEntryUi,
    totalCoeff: Double,
    onChange: (StaffEntryUi) -> Unit,
    onDelete: () -> Unit,
) {
    var nameText by remember(entry.name) { mutableStateOf(entry.name) }
    var titleText by remember(entry.titleCoeff) { mutableStateOf(formatNumberTrim(entry.titleCoeff)) }
    var dutyText by remember(entry.dutyCoeff) { mutableStateOf(formatNumberTrim(entry.dutyCoeff)) }

    LaunchedEffect(entry.name) {
        if (nameText != entry.name) nameText = entry.name
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 6.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text("${index + 1}", modifier = Modifier.width(30.dp))
        OutlinedTextField(
            value = nameText,
            onValueChange = { txt ->
                nameText = txt
                if (txt.isNotBlank()) onChange(entry.copy(name = txt.trim()))
            },
            singleLine = true,
            modifier = Modifier.weight(1.1f),
        )
        OutlinedTextField(
            value = titleText,
            onValueChange = { txt ->
                titleText = txt
                txt.toDoubleOrNull()?.let { v -> onChange(entry.copy(titleCoeff = v)) }
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            singleLine = true,
            modifier = Modifier.weight(0.8f),
        )
        OutlinedTextField(
            value = dutyText,
            onValueChange = { txt ->
                dutyText = txt
                txt.toDoubleOrNull()?.let { v -> onChange(entry.copy(dutyCoeff = v)) }
            },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            singleLine = true,
            modifier = Modifier.weight(0.8f),
        )
        Text(
            formatNumber2dp(totalCoeff),
            modifier = Modifier.width(64.dp),
            fontWeight = FontWeight.Medium,
        )
        TextButton(
            onClick = onDelete,
            modifier = Modifier.width(42.dp),
        ) {
            Text("删")
        }
    }
}
