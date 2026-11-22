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
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.lifecycleScope
import com.scluzlep.nurseocreport.ui.StaffTab
import com.scluzlep.nurseocreport.ui.WeightsTab
import com.scluzlep.nurseocreport.ui.WorkloadTab
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

    @OptIn(ExperimentalMaterial3Api::class)
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        appState = AppState(applicationContext, lifecycleScope)

        setContent {
            LaunchedEffect(Unit) {
                appState.initialize()
                handleIncomingIntent(intent)
            }

            MaterialTheme {
                Scaffold(
                    topBar = {
                        Column {
                            TopAppBar(
                                title = {
                                    Text(
                                        "护士工作量识别统计",
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                }
                            )
                            Surface(
                                tonalElevation = 2.dp,
                                modifier = Modifier.fillMaxWidth(),
                            ) {
                                Text(
                                    text = appState.statusMessage,
                                    style = MaterialTheme.typography.bodySmall,
                                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                            if (appState.busy) {
                                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                            }
                            val tabs = listOf("班次权重", "人员系数", "工作量识别")
                            TabRow(selectedTabIndex = appState.activeTab) {
                                tabs.forEachIndexed { idx, label ->
                                    Tab(
                                        selected = appState.activeTab == idx,
                                        onClick = { appState.activeTab = idx },
                                        text = { Text(label) },
                                    )
                                }
                            }
                        }
                    }
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
                                    importNamesForStaffLauncher.launch(
                                        arrayOf("text/*", "application/vnd.openxmlformats-officedocument.*")
                                    )
                                },
                            )
                            else -> WorkloadTab(
                                state = appState,
                                onTakePhoto = { startCameraCapture("workload") },
                                onPickPhoto = { pickWorkloadPhotoLauncher.launch("image/*") },
                                onImportNames = {
                                    importNamesForWorkloadLauncher.launch(
                                        arrayOf("text/*", "application/vnd.openxmlformats-officedocument.*")
                                    )
                                },
                            )
                        }
                    }
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
