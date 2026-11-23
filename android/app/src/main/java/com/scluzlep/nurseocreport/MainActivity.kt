package com.scluzlep.nurseocreport

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import com.scluzlep.nurseocreport.ui.NameImportDialog
import com.scluzlep.nurseocreport.ui.StaffTab
import com.scluzlep.nurseocreport.ui.WeightsTab
import com.scluzlep.nurseocreport.ui.WorkloadTab
import top.yukonga.miuix.kmp.basic.LinearProgressIndicator
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.SmallTopAppBar
import top.yukonga.miuix.kmp.basic.Surface
import top.yukonga.miuix.kmp.basic.TabRowWithContour
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.theme.ColorSchemeMode
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.ThemeController
import top.yukonga.miuix.kmp.utils.MiuixPopupUtils.Companion.MiuixPopupHost
import java.io.File

class MainActivity : ComponentActivity() {
    private lateinit var appState: AppState
    private var pendingCameraTarget: String = "workload"
    private var pendingCameraUri: Uri? = null

    private val cameraLauncher = registerForActivityResult(ActivityResultContracts.TakePicture()) { ok ->
        val uri = pendingCameraUri
        if (ok && uri != null) {
            if (pendingCameraTarget == "coeff") {
                appState.activeTab = 1
                appState.recognizeCoeffUri(uri)
            } else {
                appState.activeTab = 2
                appState.recognizeWorkloadUri(uri)
            }
        }
    }

    private val permissionLauncher = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            launchCameraInternal()
        } else {
            appState.statusMessage = "需要相机权限才能拍照识别"
        }
    }

    private val pickCoeffPhotoLauncher = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) {
            appState.activeTab = 1
            appState.recognizeCoeffUri(uri)
        }
    }

    private val pickWorkloadPhotoLauncher = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) {
            appState.activeTab = 2
            appState.recognizeWorkloadUri(uri)
        }
    }

    private val importNamesForStaffLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            appState.importNamesUri(uri, targetWorkloadOnly = false)
        }
    }

    private val importNamesForWorkloadLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            appState.importNamesUri(uri, targetWorkloadOnly = true)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        appState = AppState(applicationContext, lifecycleScope)

        setContent {
            val themeController = remember { ThemeController(ColorSchemeMode.System) }
            var showNameImportDialog by remember { mutableStateOf(false) }
            var importTargetWorkloadOnly by remember { mutableStateOf(false) }

            LaunchedEffect(Unit) {
                appState.initialize()
                handleIncomingIntent(intent)
            }

            MiuixTheme(controller = themeController) {
                Scaffold(
                    popupHost = { MiuixPopupHost() },
                    topBar = {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .background(MiuixTheme.colorScheme.surface),
                        ) {
                            SmallTopAppBar(
                                title = "护士工作量识别统计",
                            )
                            Surface(
                                color = MiuixTheme.colorScheme.surfaceContainer,
                                shape = RoundedCornerShape(10.dp),
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 12.dp, vertical = 2.dp),
                            ) {
                                Text(
                                    text = appState.statusMessage,
                                    style = MiuixTheme.textStyles.footnote1,
                                    color = MiuixTheme.colorScheme.onSurfaceSecondary,
                                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                            if (appState.busy) {
                                LinearProgressIndicator(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(horizontal = 12.dp, vertical = 2.dp),
                                )
                            }
                            val tabs = listOf("班次权重", "人员系数", "工作量识别")
                            TabRowWithContour(
                                tabs = tabs,
                                selectedTabIndex = appState.activeTab,
                                onTabSelected = { appState.activeTab = it },
                                modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                minWidth = 84.dp,
                                maxWidth = 160.dp,
                            )
                        }
                    },
                ) { padding ->
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(padding),
                    ) {
                        when (appState.activeTab) {
                            0 -> WeightsTab(state = appState)
                            1 -> StaffTab(
                                state = appState,
                                onTakeCoeffPhoto = { startCameraCapture("coeff") },
                                onPickCoeffPhoto = { pickCoeffPhotoLauncher.launch("image/*") },
                                onImportNames = {
                                    importTargetWorkloadOnly = false
                                    showNameImportDialog = true
                                },
                            )
                            else -> WorkloadTab(
                                state = appState,
                                onTakePhoto = { startCameraCapture("workload") },
                                onPickPhoto = { pickWorkloadPhotoLauncher.launch("image/*") },
                                onImportNames = {
                                    importTargetWorkloadOnly = true
                                    showNameImportDialog = true
                                },
                            )
                        }
                    }

                    NameImportDialog(
                        show = showNameImportDialog,
                        targetWorkloadOnly = importTargetWorkloadOnly,
                        state = appState,
                        onPickFile = {
                            val mimes = arrayOf(
                                "text/*",
                                "text/csv",
                                "text/comma-separated-values",
                                "application/csv",
                                "application/vnd.openxmlformats-officedocument.*",
                                "application/vnd.ms-excel",
                                "*/*",
                            )
                            if (importTargetWorkloadOnly) {
                                importNamesForWorkloadLauncher.launch(mimes)
                            } else {
                                importNamesForStaffLauncher.launch(mimes)
                            }
                        },
                        onDismiss = { showNameImportDialog = false },
                    )
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (::appState.isInitialized) {
            handleIncomingIntent(intent)
        }
    }

    private fun startCameraCapture(target: String) {
        pendingCameraTarget = target
        val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) {
            launchCameraInternal()
        } else {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    private fun launchCameraInternal() {
        val dir = File(cacheDir, "camera").apply { mkdirs() }
        val photoFile = File(dir, "capture_${System.currentTimeMillis()}.jpg")
        val uri = FileProvider.getUriForFile(this, "${packageName}.fileprovider", photoFile)
        pendingCameraUri = uri
        cameraLauncher.launch(uri)
    }

    private fun handleIncomingIntent(intent: Intent?) {
        if (intent == null) return
        val action = intent.action
        if (action != Intent.ACTION_SEND && action != Intent.ACTION_VIEW) return
        val uri: Uri? = if (action == Intent.ACTION_SEND) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(Intent.EXTRA_STREAM)
            }
        } else {
            intent.data
        }
        if (uri == null) return
        val mime = intent.type ?: contentResolver.getType(uri) ?: ""
        if (mime.startsWith("image/")) {
            if (appState.activeTab == 1) {
                appState.recognizeCoeffUri(uri)
            } else {
                appState.activeTab = 2
                appState.recognizeWorkloadUri(uri)
            }
        } else {
            appState.importNamesUri(uri, targetWorkloadOnly = false)
        }
    }
}
