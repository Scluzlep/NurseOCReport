package com.scluzlep.nurseocreport

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.util.Calendar
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

data class ShiftRuleUi(
    val code: String,
    val name: String,
    val weight: Double,
    val enabled: Boolean,
)

data class StaffEntryUi(
    val name: String,
    val titleCoeff: Double,
    val dutyCoeff: Double,
)

data class PointUi(
    val x: Float,
    val y: Float,
)

data class WorkloadCellUi(
    val row: Int,
    val week: Int,
    val shiftCol: Int,
    val isBlank: Boolean,
    val value: Double,
    val confidence: Float,
    val status: Int,
    val reviewReason: String,
    val quad: List<PointUi>,
)

data class WorkloadRowUi(
    val row: Int,
    val ocrName: String,
    val staffName: String,
    val nameConfidence: Float,
    val nameNeedsReview: Boolean,
    val nameQuad: List<PointUi>,
    val cells: List<WorkloadCellUi>,
)

data class WorkloadSheetUi(
    val year: Int,
    val month: Int,
    val weeks: Int,
    val weekLabels: List<String>,
    val columnShiftCodes: List<String>,
    val columnShiftNames: List<String>,
    val appliedRotationDeg: Int,
    val imgWidth: Int,
    val imgHeight: Int,
    val corners: List<PointUi>,
    val rows: List<WorkloadRowUi>,
)

data class PersonSummaryUi(
    val name: String,
    val titleCoeff: Double,
    val dutyCoeff: Double,
    val effectiveCoeff: Double,
    val matchedInStaff: Boolean,
    val shiftTotals: List<Double>,
    val weightedSum: Double,
    val finalScore: Double,
)

data class SheetSummaryUi(
    val pendingReviewCount: Int,
    val alerts: List<String>,
    val shiftGrandTotals: List<Double>,
    val weightedGrandTotal: Double,
    val finalGrandTotal: Double,
    val people: List<PersonSummaryUi>,
)

fun formatNumber2dp(v: Double): String {
    val clean = if (abs(v) < 1e-9) 0.0 else v
    return String.format(Locale.US, "%.2f", clean)
}

fun formatNumberTrim(v: Double): String {
    if (abs(v) < 1e-9) return "0"
    val rounded = (v * 100.0).roundToInt() / 100.0
    var s = String.format(Locale.US, "%.2f", rounded)
    if (s.contains('.')) {
        s = s.trimEnd('0').trimEnd('.')
    }
    return s
}

class AppState(
    private val context: Context,
    private val scope: CoroutineScope,
) {
    private val configFile = File(context.filesDir, "config.json")

    var activeTab by mutableIntStateOf(2)
    var modelsReady by mutableStateOf(false)
    var busy by mutableStateOf(false)
    var statusMessage by mutableStateOf("就绪")

    var enableTitleCoeff by mutableStateOf(true)
    var enableDutyCoeff by mutableStateOf(true)
    val shiftRules = mutableStateListOf<ShiftRuleUi>()
    val staffList = mutableStateListOf<StaffEntryUi>()

    var year by mutableIntStateOf(Calendar.getInstance().get(Calendar.YEAR))
    var month by mutableIntStateOf(Calendar.getInstance().get(Calendar.MONTH) + 1)

    var sheet by mutableStateOf<WorkloadSheetUi?>(null)
    var summary by mutableStateOf<SheetSummaryUi?>(null)

    var workingBitmap by mutableStateOf<Bitmap?>(null)
    var cellPreviewBitmap by mutableStateOf<Bitmap?>(null)
    var cornerEditMode by mutableStateOf(false)
    val editableCorners = mutableStateListOf<PointUi>()

    var selectedRow by mutableIntStateOf(-1)
    var selectedColIndex by mutableIntStateOf(-2)

    fun initialize() {
        scope.launch {
            busy = true
            statusMessage = "正在初始化识别模型"
            val initOk = withContext(Dispatchers.IO) {
                NativeBridge.nativeInit(context.assets)
            }
            modelsReady = initOk

            val savedJson = withContext(Dispatchers.IO) {
                if (configFile.isFile) {
                    runCatching { configFile.readText(Charsets.UTF_8) }.getOrNull()
                } else {
                    null
                }
            } ?: withContext(Dispatchers.IO) {
                NativeBridge.nativeGetDefaultConfigJson()
            }

            applyConfigFromJsonText(savedJson)
            syncConfigToNative(saveToDisk = !configFile.isFile)

            statusMessage = if (initOk) "就绪" else "模型加载失败"
            busy = false
        }
    }

    fun computeEffectiveCoeff(entry: StaffEntryUi): Double {
        if (!enableTitleCoeff && !enableDutyCoeff) return 1.0
        var total = 0.0
        if (enableTitleCoeff) total += entry.titleCoeff
        if (enableDutyCoeff) total += entry.dutyCoeff
        return total
    }

    fun setTitleCoeffEnabled(enabled: Boolean) {
        enableTitleCoeff = enabled
        syncConfigToNative(saveToDisk = true)
    }

    fun setDutyCoeffEnabled(enabled: Boolean) {
        enableDutyCoeff = enabled
        syncConfigToNative(saveToDisk = true)
    }

    fun updateShiftRule(index: Int, updated: ShiftRuleUi) {
        if (index !in shiftRules.indices) return
        shiftRules[index] = updated
        syncConfigToNative(saveToDisk = true)
    }

    fun addShiftRule(name: String, weight: Double) {
        val trimmed = name.trim()
        if (trimmed.isEmpty()) return
        shiftRules.add(ShiftRuleUi(code = trimmed, name = trimmed, weight = weight, enabled = true))
        syncConfigToNative(saveToDisk = true)
    }

    fun removeShiftRule(index: Int) {
        if (index !in shiftRules.indices) return
        shiftRules.removeAt(index)
        syncConfigToNative(saveToDisk = true)
    }

    fun resetDefaultShiftRules() {
        scope.launch {
            val defJson = withContext(Dispatchers.IO) {
                NativeBridge.nativeGetDefaultConfigJson()
            }
            val obj = runCatching { JSONObject(defJson) }.getOrNull() ?: return@launch
            val rulesArr = obj.optJSONArray("shiftRules") ?: return@launch
            shiftRules.clear()
            for (i in 0 until rulesArr.length()) {
                val r = rulesArr.getJSONObject(i)
                shiftRules.add(
                    ShiftRuleUi(
                        code = r.optString("code", ""),
                        name = r.optString("name", ""),
                        weight = r.optDouble("weight", 1.0),
                        enabled = r.optBoolean("enabled", true),
                    )
                )
            }
            val flags = obj.optJSONObject("coeffFlags")
            if (flags != null) {
                enableTitleCoeff = flags.optBoolean("enableTitleCoeff", true)
                enableDutyCoeff = flags.optBoolean("enableDutyCoeff", true)
            }
            syncConfigToNative(saveToDisk = true)
            statusMessage = "已恢复默认班次权重"
        }
    }

    fun updateStaffEntry(index: Int, updated: StaffEntryUi) {
        if (index !in staffList.indices) return
        staffList[index] = updated
        syncConfigToNative(saveToDisk = true)
    }

    fun addStaffEntry(name: String, titleCoeff: Double, dutyCoeff: Double) {
        val trimmed = name.trim()
        if (trimmed.isEmpty()) return
        staffList.add(StaffEntryUi(name = trimmed, titleCoeff = titleCoeff, dutyCoeff = dutyCoeff))
        syncConfigToNative(saveToDisk = true)
    }

    fun removeStaffEntry(index: Int) {
        if (index !in staffList.indices) return
        staffList.removeAt(index)
        syncConfigToNative(saveToDisk = true)
    }

    fun clearStaffList() {
        staffList.clear()
        syncConfigToNative(saveToDisk = true)
        statusMessage = "已清空人员系数表"
    }

    fun recognizeCoeffUri(uri: Uri) {
        if (!modelsReady) {
            statusMessage = "模型尚未就绪"
            return
        }
        scope.launch {
            busy = true
            statusMessage = "正在识别人员系数表"
            val bytes = withContext(Dispatchers.IO) { readUriBytes(uri) }
            if (bytes == null || bytes.isEmpty()) {
                statusMessage = "无法读取所选图片"
                busy = false
                return@launch
            }
            val respStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeProcessCoeffBytes(bytes)
            }
            val obj = runCatching { JSONObject(respStr) }.getOrNull()
            if (obj == null || !obj.optBoolean("ok", false)) {
                statusMessage = obj?.optString("error", "人员系数识别失败") ?: "人员系数识别失败"
                busy = false
                return@launch
            }
            val rows = obj.optJSONArray("rows") ?: JSONArray()
            staffList.clear()
            var reviewCount = 0
            for (i in 0 until rows.length()) {
                val r = rows.getJSONObject(i)
                val matchedName = r.optString("matchedName", r.optString("ocrName", ""))
                val titleCoeff = r.optDouble("titleCoeff", 1.0)
                val dutyCoeff = r.optDouble("dutyCoeff", 0.0)
                if (r.optBoolean("needsReview", false)) {
                    reviewCount += 1
                }
                if (matchedName.isNotBlank()) {
                    staffList.add(StaffEntryUi(matchedName, titleCoeff, dutyCoeff))
                }
            }
            syncConfigToNative(saveToDisk = true)
            statusMessage = if (reviewCount > 0) {
                "已识别 ${staffList.size} 人，建议核对 $reviewCount 行"
            } else {
                "已识别 ${staffList.size} 人并保存"
            }
            busy = false
        }
    }

    fun recognizeWorkloadUri(uri: Uri) {
        if (!modelsReady) {
            statusMessage = "模型尚未就绪"
            return
        }
        scope.launch {
            busy = true
            statusMessage = "正在识别工作量表"
            val bytes = withContext(Dispatchers.IO) { readUriBytes(uri) }
            if (bytes == null || bytes.isEmpty()) {
                statusMessage = "无法读取所选图片"
                busy = false
                return@launch
            }
            val respStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeProcessWorkloadBytes(bytes, year, month)
            }
            handleWorkloadResponse(respStr, reloadImage = true)
            busy = false
        }
    }

    fun rotateWorkload(quarterTurnsCw: Int) {
        if (sheet == null && workingBitmap == null) {
            statusMessage = "请先加载工作量照片"
            return
        }
        scope.launch {
            busy = true
            statusMessage = "正在旋转并重新识别"
            val respStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeRotateWorkload(quarterTurnsCw, year, month)
            }
            handleWorkloadResponse(respStr, reloadImage = true)
            busy = false
        }
    }

    fun toggleCornerEditMode() {
        val currentSheet = sheet ?: return
        if (!cornerEditMode) {
            editableCorners.clear()
            editableCorners.addAll(currentSheet.corners)
            cornerEditMode = editableCorners.size == 4
        } else {
            cornerEditMode = false
        }
    }

    fun moveCorner(index: Int, pt: PointUi) {
        if (index in 0 until editableCorners.size) {
            val w = (sheet?.imgWidth ?: 1).toFloat()
            val h = (sheet?.imgHeight ?: 1).toFloat()
            editableCorners[index] = PointUi(
                x = pt.x.coerceIn(0f, w),
                y = pt.y.coerceIn(0f, h),
            )
        }
    }

    fun applyAdjustedCorners() {
        if (editableCorners.size != 4) return
        val flat = FloatArray(8)
        for (i in 0 until 4) {
            flat[i * 2] = editableCorners[i].x
            flat[i * 2 + 1] = editableCorners[i].y
        }
        scope.launch {
            busy = true
            statusMessage = "正在按新边框重新识别"
            val respStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeUpdateWorkloadCorners(flat, year, month)
            }
            cornerEditMode = false
            handleWorkloadResponse(respStr, reloadImage = false)
            busy = false
        }
    }

    fun importNamesUri(uri: Uri, targetWorkloadOnly: Boolean) {
        scope.launch {
            busy = true
            statusMessage = "正在解析名单文件"
            val (filename, rawBytes) = withContext(Dispatchers.IO) {
                val name = resolveDisplayName(uri) ?: "names.txt"
                val data = readUriBytes(uri)
                name to data
            }
            if (rawBytes == null || rawBytes.isEmpty()) {
                statusMessage = "无法读取名单文件"
                busy = false
                return@launch
            }
            val preparedBytes = normalizePlainTextEncoding(filename, rawBytes)
            val parseStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeParseNamesBytes(filename, preparedBytes)
            }
            val parseObj = runCatching { JSONObject(parseStr) }.getOrNull()
            if (parseObj == null || !parseObj.optBoolean("ok", false)) {
                statusMessage = parseObj?.optString("error", "名单解析失败") ?: "名单解析失败"
                busy = false
                return@launch
            }
            val namesArr = parseObj.optJSONArray("names") ?: JSONArray()
            if (namesArr.length() == 0) {
                statusMessage = "名单中未提取到有效姓名"
                busy = false
                return@launch
            }
            val namesJson = namesArr.toString()

            if (targetWorkloadOnly) {
                val wlStr = withContext(Dispatchers.IO) {
                    NativeBridge.nativeApplyNamesToWorkload(namesJson)
                }
                handleWorkloadResponse(wlStr, reloadImage = false)
                if (sheet != null) {
                    statusMessage = "已用 ${namesArr.length()} 个标准姓名校准工作量表"
                }
            } else {
                val alignStr = withContext(Dispatchers.IO) {
                    NativeBridge.nativeAlignStaffWithNames(namesJson)
                }
                val alignObj = runCatching { JSONObject(alignStr) }.getOrNull()
                if (alignObj == null || !alignObj.optBoolean("ok", false)) {
                    statusMessage = alignObj?.optString("error", "姓名校准失败") ?: "姓名校准失败"
                } else {
                    alignObj.optJSONObject("config")?.let { cfgObj ->
                        applyConfigFromJsonText(cfgObj.toString())
                        withContext(Dispatchers.IO) {
                            runCatching { configFile.writeText(serializeConfigJson(), Charsets.UTF_8) }
                        }
                    }
                    if (alignObj.has("sheet") && alignObj.has("summary")) {
                        updateSheetAndSummaryFromJson(alignObj)
                    }
                    statusMessage = "已用 ${namesArr.length()} 个标准姓名完成校准"
                }
            }
            busy = false
        }
    }

    fun selectCell(row: Int, colIndex: Int) {
        selectedRow = row
        selectedColIndex = colIndex
        refreshSelectedPreview()
    }

    fun selectNextPendingReview() {
        val s = sheet ?: return
        val totalRows = s.rows.size
        if (totalRows == 0) return

        val items = mutableListOf<Pair<Int, Int>>()
        for (r in 0 until totalRows) {
            val rowObj = s.rows[r]
            if (rowObj.nameNeedsReview) {
                items.add(r to -1)
            }
            for (c in rowObj.cells.indices) {
                if (rowObj.cells[c].status == 1) {
                    items.add(r to c)
                }
            }
        }
        if (items.isEmpty()) {
            statusMessage = "当前没有待确认单元格"
            return
        }
        val curIdx = items.indexOfFirst { it.first == selectedRow && it.second == selectedColIndex }
        val next = items[(curIdx + 1) % items.size]
        selectCell(next.first, next.second)
    }

    fun editCell(row: Int, colIndex: Int, isBlank: Boolean, value: Double, nameText: String?) {
        scope.launch {
            val respStr = withContext(Dispatchers.IO) {
                NativeBridge.nativeEditCell(row, colIndex, isBlank, value, nameText)
            }
            val obj = runCatching { JSONObject(respStr) }.getOrNull()
            if (obj != null && obj.optBoolean("ok", false)) {
                updateSheetAndSummaryFromJson(obj)
                refreshSelectedPreview()
            } else if (obj != null) {
                statusMessage = obj.optString("error", "修改失败")
            }
        }
    }

    fun confirmAndAdvance() {
        val r = selectedRow
        val c = selectedColIndex
        val s = sheet ?: return
        if (r !in s.rows.indices) return
        scope.launch {
            val respStr = withContext(Dispatchers.IO) {
                if (c < 0) {
                    NativeBridge.nativeEditCell(r, -1, false, -1.0, s.rows[r].staffName)
                } else {
                    NativeBridge.nativeEditCell(r, c, false, -1.0, null)
                }
            }
            val obj = runCatching { JSONObject(respStr) }.getOrNull()
            if (obj != null && obj.optBoolean("ok", false)) {
                updateSheetAndSummaryFromJson(obj)
                selectNextPendingReview()
            }
        }
    }

    fun exportAndShareXlsx() {
        if (sheet == null) {
            statusMessage = "请先识别工作量照片"
            return
        }
        scope.launch {
            busy = true
            statusMessage = "正在生成统计表格"
            val exportDir = File(context.cacheDir, "exports").apply { mkdirs() }
            val filename = String.format(Locale.US, "工作量统计_%04d年%02d月.xlsx", year, month)
            val outFile = File(exportDir, filename)
            val err = withContext(Dispatchers.IO) {
                NativeBridge.nativeExportXlsx(outFile.absolutePath, year, month)
            }
            busy = false
            if (err.isNotBlank()) {
                statusMessage = err
                return@launch
            }
            statusMessage = "已生成 $filename"
            val shareUri = FileProvider.getUriForFile(
                context,
                "${context.packageName}.fileprovider",
                outFile,
            )
            val sendIntent = Intent(Intent.ACTION_SEND).apply {
                type = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
                putExtra(Intent.EXTRA_STREAM, shareUri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            val chooser = Intent.createChooser(sendIntent, "分享工作量统计表")
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(chooser)
        }
    }

    private fun refreshSelectedPreview() {
        val s = sheet
        val r = selectedRow
        val c = selectedColIndex
        if (s == null || r !in s.rows.indices || (c >= 0 && c !in s.rows[r].cells.indices)) {
            cellPreviewBitmap = null
            return
        }
        scope.launch {
            val bmp = withContext(Dispatchers.IO) {
                val w = if (c < 0) 240 else 160
                val h = 96
                val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                NativeBridge.nativeRenderCellPreview(r, c, out)
                out
            }
            cellPreviewBitmap = bmp
        }
    }

    private suspend fun handleWorkloadResponse(respStr: String, reloadImage: Boolean) {
        val obj = runCatching { JSONObject(respStr) }.getOrNull()
        if (obj == null || !obj.optBoolean("ok", false)) {
            statusMessage = obj?.optString("error", "识别失败") ?: "识别失败"
            return
        }
        updateSheetAndSummaryFromJson(obj)
        if (reloadImage) {
            workingBitmap = withContext(Dispatchers.IO) {
                val sz = NativeBridge.nativeGetWorkingImageSize()
                if (sz.size >= 2 && sz[0] > 0 && sz[1] > 0) {
                    val maxDim = 1600f
                    val ratio = minOf(1f, maxDim / maxOf(sz[0], sz[1]).toFloat())
                    val bw = maxOf(1, (sz[0] * ratio).roundToInt())
                    val bh = maxOf(1, (sz[1] * ratio).roundToInt())
                    val bmp = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888)
                    NativeBridge.nativeRenderWorkingImage(bmp)
                    bmp
                } else {
                    null
                }
            }
        }
        val cur = sheet
        if (cur != null && cur.rows.isNotEmpty()) {
            selectNextPendingReview()
            if (selectedRow !in cur.rows.indices) {
                selectCell(0, 0)
            }
            val pending = summary?.pendingReviewCount ?: 0
            statusMessage = if (pending > 0) {
                "识别完成，共有 $pending 处待确认"
            } else {
                "识别完成，共 ${cur.rows.size} 人"
            }
        }
    }

    private fun updateSheetAndSummaryFromJson(root: JSONObject) {
        if (root.has("sheet")) {
            sheet = parseSheet(root.getJSONObject("sheet"))
        }
        if (root.has("summary")) {
            summary = parseSummary(root.getJSONObject("summary"))
        }
    }

    private fun syncConfigToNative(saveToDisk: Boolean) {
        val jsonText = serializeConfigJson()
        scope.launch {
            val resp = withContext(Dispatchers.IO) {
                if (saveToDisk) {
                    runCatching { configFile.writeText(jsonText, Charsets.UTF_8) }
                }
                NativeBridge.nativeSetConfigJson(jsonText)
            }
            val obj = runCatching { JSONObject(resp) }.getOrNull()
            if (obj != null && obj.optBoolean("ok", false) && obj.has("summary")) {
                updateSheetAndSummaryFromJson(obj)
            }
        }
    }

    private fun serializeConfigJson(): String {
        val root = JSONObject()
        val flags = JSONObject()
        flags.put("enableTitleCoeff", enableTitleCoeff)
        flags.put("enableDutyCoeff", enableDutyCoeff)
        root.put("coeffFlags", flags)

        val rulesArr = JSONArray()
        for (r in shiftRules) {
            val jo = JSONObject()
            jo.put("code", r.code)
            jo.put("name", r.name)
            jo.put("weight", r.weight)
            jo.put("enabled", r.enabled)
            rulesArr.put(jo)
        }
        root.put("shiftRules", rulesArr)

        val staffArr = JSONArray()
        for (s in staffList) {
            val jo = JSONObject()
            jo.put("name", s.name)
            jo.put("titleCoeff", s.titleCoeff)
            jo.put("dutyCoeff", s.dutyCoeff)
            staffArr.put(jo)
        }
        root.put("staff", staffArr)
        return root.toString(2)
    }

    private fun applyConfigFromJsonText(jsonText: String) {
        val obj = runCatching { JSONObject(jsonText) }.getOrNull() ?: return
        val flags = obj.optJSONObject("coeffFlags")
        if (flags != null) {
            enableTitleCoeff = flags.optBoolean("enableTitleCoeff", true)
            enableDutyCoeff = flags.optBoolean("enableDutyCoeff", true)
        }
        val rulesArr = obj.optJSONArray("shiftRules")
        if (rulesArr != null && rulesArr.length() > 0) {
            shiftRules.clear()
            for (i in 0 until rulesArr.length()) {
                val r = rulesArr.getJSONObject(i)
                val name = r.optString("name", "")
                if (name.isNotEmpty()) {
                    shiftRules.add(
                        ShiftRuleUi(
                            code = r.optString("code", ""),
                            name = name,
                            weight = r.optDouble("weight", 1.0),
                            enabled = r.optBoolean("enabled", true),
                        )
                    )
                }
            }
        }
        val staffArr = obj.optJSONArray("staff")
        staffList.clear()
        if (staffArr != null) {
            for (i in 0 until staffArr.length()) {
                val s = staffArr.getJSONObject(i)
                val name = s.optString("name", "")
                if (name.isNotEmpty()) {
                    staffList.add(
                        StaffEntryUi(
                            name = name,
                            titleCoeff = s.optDouble("titleCoeff", 1.0),
                            dutyCoeff = s.optDouble("dutyCoeff", 0.0),
                        )
                    )
                }
            }
        }
    }

    private fun parseQuads(arr: JSONArray?): List<PointUi> {
        if (arr == null) return emptyList()
        val out = ArrayList<PointUi>(arr.length())
        for (i in 0 until arr.length()) {
            val p = arr.getJSONObject(i)
            out.add(PointUi(p.optDouble("x", 0.0).toFloat(), p.optDouble("y", 0.0).toFloat()))
        }
        return out
    }

    private fun parseStrings(arr: JSONArray?): List<String> {
        if (arr == null) return emptyList()
        val out = ArrayList<String>(arr.length())
        for (i in 0 until arr.length()) {
            out.add(arr.optString(i, ""))
        }
        return out
    }

    private fun parseDoubles(arr: JSONArray?): List<Double> {
        if (arr == null) return emptyList()
        val out = ArrayList<Double>(arr.length())
        for (i in 0 until arr.length()) {
            out.add(arr.optDouble(i, 0.0))
        }
        return out
    }

    private fun parseSheet(jo: JSONObject): WorkloadSheetUi {
        val geom = jo.optJSONObject("geometry")
        val rowsArr = jo.optJSONArray("rows") ?: JSONArray()
        val rows = ArrayList<WorkloadRowUi>(rowsArr.length())
        for (i in 0 until rowsArr.length()) {
            val jr = rowsArr.getJSONObject(i)
            val cellsArr = jr.optJSONArray("cells") ?: JSONArray()
            val cells = ArrayList<WorkloadCellUi>(cellsArr.length())
            for (c in 0 until cellsArr.length()) {
                val jc = cellsArr.getJSONObject(c)
                cells.add(
                    WorkloadCellUi(
                        row = jc.optInt("row", i),
                        week = jc.optInt("week", 0),
                        shiftCol = jc.optInt("shiftCol", 0),
                        isBlank = jc.optBoolean("isBlank", true),
                        value = jc.optDouble("value", 0.0),
                        confidence = jc.optDouble("confidence", 0.0).toFloat(),
                        status = jc.optInt("status", 0),
                        reviewReason = jc.optString("reviewReason", ""),
                        quad = parseQuads(jc.optJSONArray("quad")),
                    )
                )
            }
            rows.add(
                WorkloadRowUi(
                    row = jr.optInt("row", i),
                    ocrName = jr.optString("ocrName", ""),
                    staffName = jr.optString("staffName", ""),
                    nameConfidence = jr.optDouble("nameConfidence", 0.0).toFloat(),
                    nameNeedsReview = jr.optBoolean("nameNeedsReview", false),
                    nameQuad = parseQuads(jr.optJSONArray("nameQuad")),
                    cells = cells,
                )
            )
        }
        return WorkloadSheetUi(
            year = jo.optInt("year", year),
            month = jo.optInt("month", month),
            weeks = jo.optInt("weeks", 5),
            weekLabels = parseStrings(jo.optJSONArray("weekLabels")),
            columnShiftCodes = parseStrings(jo.optJSONArray("columnShiftCodes")),
            columnShiftNames = parseStrings(jo.optJSONArray("columnShiftNames")),
            appliedRotationDeg = jo.optInt("appliedRotationDeg", 0),
            imgWidth = geom?.optInt("imgWidth", 0) ?: 0,
            imgHeight = geom?.optInt("imgHeight", 0) ?: 0,
            corners = parseQuads(geom?.optJSONArray("corners")),
            rows = rows,
        )
    }

    private fun parseSummary(jo: JSONObject): SheetSummaryUi {
        val peopleArr = jo.optJSONArray("people") ?: JSONArray()
        val people = ArrayList<PersonSummaryUi>(peopleArr.length())
        for (i in 0 until peopleArr.length()) {
            val jp = peopleArr.getJSONObject(i)
            people.add(
                PersonSummaryUi(
                    name = jp.optString("name", ""),
                    titleCoeff = jp.optDouble("titleCoeff", 1.0),
                    dutyCoeff = jp.optDouble("dutyCoeff", 0.0),
                    effectiveCoeff = jp.optDouble("effectiveCoeff", 1.0),
                    matchedInStaff = jp.optBoolean("matchedInStaff", false),
                    shiftTotals = parseDoubles(jp.optJSONArray("shiftTotals")),
                    weightedSum = jp.optDouble("weightedSum", 0.0),
                    finalScore = jp.optDouble("finalScore", 0.0),
                )
            )
        }
        return SheetSummaryUi(
            pendingReviewCount = jo.optInt("pendingReviewCount", 0),
            alerts = parseStrings(jo.optJSONArray("alerts")),
            shiftGrandTotals = parseDoubles(jo.optJSONArray("shiftGrandTotals")),
            weightedGrandTotal = jo.optDouble("weightedGrandTotal", 0.0),
            finalGrandTotal = jo.optDouble("finalGrandTotal", 0.0),
            people = people,
        )
    }

    private fun readUriBytes(uri: Uri): ByteArray? {
        return runCatching {
            context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
        }.getOrNull()
    }

    private fun resolveDisplayName(uri: Uri): String? {
        if (uri.scheme == "file") {
            return uri.lastPathSegment
        }
        runCatching {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst()) {
                        val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                        if (idx >= 0) return cursor.getString(idx)
                    }
                }
        }
        return uri.lastPathSegment
    }

    private fun normalizePlainTextEncoding(filename: String, raw: ByteArray): ByteArray {
        val lower = filename.lowercase(Locale.ROOT)
        val isZip = raw.size >= 4 && raw[0] == 0x50.toByte() && raw[1] == 0x4B.toByte()
        if (isZip || lower.endsWith(".docx") || lower.endsWith(".xlsx")) {
            return raw
        }
        val hasUtf8Bom = raw.size >= 3 &&
            raw[0] == 0xEF.toByte() && raw[1] == 0xBB.toByte() && raw[2] == 0xBF.toByte()
        if (hasUtf8Bom) return raw
        return try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(raw))
            raw
        } catch (_: CharacterCodingException) {
            val gbk = runCatching {
                Charset.forName("GB18030").decode(ByteBuffer.wrap(raw)).toString()
            }.getOrNull()
            gbk?.toByteArray(Charsets.UTF_8) ?: raw
        }
    }
}
