package com.scluzlep.nurseocreport

import android.content.res.AssetManager
import android.graphics.Bitmap

object NativeBridge {
    init {
        System.loadLibrary("nurseocr_jni")
    }

    external fun nativeInit(assetManager: AssetManager): Boolean
    external fun nativeGetDefaultConfigJson(): String
    external fun nativeSetConfigJson(configJson: String): String
    external fun nativeParseNamesText(rawText: String): String
    external fun nativeParseNamesBytes(filenameHint: String, fileBytes: ByteArray): String
    external fun nativeAlignStaffWithNames(namesJsonArray: String): String
    external fun nativeApplyNamesToWorkload(namesJsonArray: String): String
    external fun nativeProcessCoeffBytes(imgBytes: ByteArray): String
    external fun nativeProcessWorkloadBytes(imgBytes: ByteArray, year: Int, month: Int): String
    external fun nativeRotateWorkload(quarterTurnsCw: Int, year: Int, month: Int): String
    external fun nativeUpdateWorkloadCorners(flat8: FloatArray, year: Int, month: Int): String
    external fun nativeEditCell(row: Int, colIndex: Int, isBlank: Boolean, value: Double, nameText: String?): String
    external fun nativeDeleteWorkloadRow(rowIndex: Int): String
    external fun nativeAddWorkloadRow(staffName: String): String
    external fun nativeGetWorkingImageSize(): IntArray
    external fun nativeRenderWorkingImage(outBitmap: Bitmap)
    external fun nativeRenderCellPreview(row: Int, colIndex: Int, outBitmap: Bitmap)
    external fun nativeExportXlsx(outPath: String, year: Int, month: Int): String
}
