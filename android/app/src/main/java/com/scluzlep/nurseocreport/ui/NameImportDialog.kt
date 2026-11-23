package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import com.scluzlep.nurseocreport.AppState
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.ButtonDefaults
import top.yukonga.miuix.kmp.basic.HorizontalDivider
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextButton
import top.yukonga.miuix.kmp.basic.TextField
import top.yukonga.miuix.kmp.extra.SuperDialog
import top.yukonga.miuix.kmp.theme.MiuixTheme

@Composable
fun NameImportDialog(
    show: Boolean,
    targetWorkloadOnly: Boolean,
    state: AppState,
    onPickFile: () -> Unit,
    onDismiss: () -> Unit,
) {
    var rawText by remember { mutableStateOf("") }
    LaunchedEffect(show) {
        if (show) {
            rawText = ""
        }
    }

    val parsedNames = remember(rawText) {
        state.parseNamesFromText(rawText)
    }

    SuperDialog(
        show = show,
        title = if (targetWorkloadOnly) "导入名单校准工作量姓名" else "导入标准人员名单",
        summary = "支持直接粘贴姓名文本，或从文档文件导入",
        onDismissRequest = onDismiss,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TextField(
                value = rawText,
                onValueChange = { rawText = it },
                label = "粘贴姓名（支持空格 / 、 / ， / , / 换行分隔）",
                useLabelAsPlaceholder = true,
                insideMargin = DpSize(10.dp, 10.dp),
                singleLine = false,
                minLines = 3,
                maxLines = 6,
                modifier = Modifier.fillMaxWidth(),
            )

            Text(
                text = if (parsedNames.isEmpty()) {
                    "尚未解析到有效姓名（2~6个汉字）"
                } else {
                    val preview = parsedNames.take(10).joinToString("、") +
                        if (parsedNames.size > 10) " 等" else ""
                    "已解析 ${parsedNames.size} 人：$preview"
                },
                style = MiuixTheme.textStyles.footnote1,
                color = if (parsedNames.isEmpty()) {
                    MiuixTheme.colorScheme.onSurfaceVariantSummary
                } else {
                    MiuixTheme.colorScheme.primary
                },
                fontWeight = FontWeight.Medium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )

            Button(
                onClick = {
                    state.importNamesList(parsedNames, targetWorkloadOnly = targetWorkloadOnly)
                    onDismiss()
                },
                enabled = parsedNames.isNotEmpty() && !state.busy,
                colors = ButtonDefaults.buttonColorsPrimary(),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = "确认导入粘贴名单（${parsedNames.size}人）",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            HorizontalDivider(modifier = Modifier.padding(vertical = 2.dp))

            Button(
                onClick = {
                    onDismiss()
                    onPickFile()
                },
                enabled = !state.busy,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = "从文件导入 (.txt/.csv/.docx/.xlsx)",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                TextButton(
                    text = "取消",
                    onClick = onDismiss,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}
