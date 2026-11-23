package com.scluzlep.nurseocreport.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import top.yukonga.miuix.kmp.basic.Button
import top.yukonga.miuix.kmp.basic.ButtonDefaults
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.theme.MiuixTheme

@Composable
fun UniformToolbarButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    primary: Boolean = false,
    danger: Boolean = false,
) {
    val colors = when {
        primary -> ButtonDefaults.buttonColorsPrimary()
        danger -> ButtonDefaults.buttonColors(
            color = MiuixTheme.colorScheme.errorContainer,
            contentColor = MiuixTheme.colorScheme.error,
        )
        else -> ButtonDefaults.buttonColors()
    }
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = colors,
        cornerRadius = 11.dp,
        minWidth = 0.dp,
        minHeight = 38.dp,
        insideMargin = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
        modifier = modifier.height(38.dp),
    ) {
        Text(
            text = text,
            style = MiuixTheme.textStyles.body2,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun UniformQuickButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    primary: Boolean = false,
) {
    val colors = if (primary) {
        ButtonDefaults.buttonColorsPrimary()
    } else {
        ButtonDefaults.buttonColors()
    }
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = colors,
        cornerRadius = 10.dp,
        minWidth = 0.dp,
        minHeight = 36.dp,
        insideMargin = PaddingValues(horizontal = 4.dp, vertical = 0.dp),
        modifier = modifier.height(36.dp),
    ) {
        Text(
            text = text,
            style = MiuixTheme.textStyles.body2,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun UniformDialogButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    primary: Boolean = false,
    danger: Boolean = false,
) {
    val colors = when {
        primary -> ButtonDefaults.buttonColorsPrimary()
        danger -> ButtonDefaults.buttonColors(
            color = MiuixTheme.colorScheme.errorContainer,
            contentColor = MiuixTheme.colorScheme.error,
        )
        else -> ButtonDefaults.buttonColors()
    }
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = colors,
        cornerRadius = 12.dp,
        minWidth = 0.dp,
        minHeight = 42.dp,
        insideMargin = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
        modifier = modifier.height(42.dp),
    ) {
        Text(
            text = text,
            style = MiuixTheme.textStyles.button,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
            softWrap = false,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun ToolbarGridRow(
    slotCount: Int = 4,
    content: List<@Composable RowScope.() -> Unit>,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        for (i in 0 until slotCount) {
            if (i < content.size) {
                content[i](this)
            } else {
                Spacer(modifier = Modifier.weight(1f))
            }
        }
    }
}
