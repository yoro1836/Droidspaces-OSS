package com.droidspaces.app.ui.component

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Restore
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import com.droidspaces.app.R
import com.droidspaces.app.util.Constants
import com.droidspaces.app.util.ContainerCommandBuilder
import com.droidspaces.app.util.ContainerInfo
import com.droidspaces.app.util.ContainerManager
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** One line of `droidspaces snapshots`: number, time, MiB written since, label. */
private data class Snapshot(val number: Int, val time: String, val sizeMb: String, val label: String)

private enum class SnapshotAction { RESTORE, DELETE }

/**
 * Snapshots of an image container: take one, go back to one, delete one. The backend
 * only does these on a stopped container, so a running one gets the list and a hint.
 */
@Composable
fun SnapshotsDialog(container: ContainerInfo, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var snapshots by remember { mutableStateOf<List<Snapshot>>(emptyList()) }
    var running by remember { mutableStateOf(container.isRunning) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var label by remember { mutableStateOf("") }
    var pending by remember { mutableStateOf<Pair<SnapshotAction, Snapshot>?>(null) }

    val base = "\"${Constants.DROIDSPACES_BINARY_PATH}\" " +
        "--config=${ContainerCommandBuilder.quote(ContainerCommandBuilder.getConfigPath(container))}"

    suspend fun reload() {
        running = ContainerManager.checkContainerStatus(container.name).first
        snapshots = withContext(Dispatchers.IO) {
            Shell.cmd("$base snapshots 2>/dev/null").exec().out.mapNotNull { line ->
                val f = line.split('\t', limit = 4)
                val number = f.getOrNull(0)?.toIntOrNull() ?: return@mapNotNull null
                Snapshot(number, f.getOrElse(1) { "" }, f.getOrElse(2) { "0" }, f.getOrElse(3) { "" })
            }
        }
    }

    fun run(args: String) {
        scope.launch {
            busy = true
            error = null
            val result = withContext(Dispatchers.IO) { Shell.cmd("$base $args 2>&1").exec() }
            if (!result.isSuccess) error = result.out.lastOrNull { it.isNotBlank() }
            reload()
            busy = false
        }
    }

    LaunchedEffect(container.name) { reload() }

    DsDialog(
        onDismiss = { if (!busy) onDismiss() },
        footer = {
            DialogFooterRow(
                dismissLabel = stringResource(R.string.close),
                confirmLabel = stringResource(R.string.snapshot_take),
                onDismiss = { if (!busy) onDismiss() },
                onConfirm = {
                    // The label goes to a root shell: quote() is the choke point for that.
                    run("snapshot ${ContainerCommandBuilder.quote(label.trim().ifEmpty { "snapshot" })}")
                    label = ""
                },
                confirmEnabled = !busy && !running
            )
        }
    ) {
        Text(
            text = stringResource(R.string.snapshots_title, container.name),
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold
        )
        Text(
            text = stringResource(
                when {
                    busy -> R.string.snapshots_working
                    running -> R.string.snapshots_stop_first
                    else -> R.string.snapshots_body
                }
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        error?.let {
            Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
        }
        OutlinedTextField(
            value = label,
            onValueChange = { label = it.take(40) },
            label = { Text(stringResource(R.string.snapshot_label)) },
            singleLine = true,
            enabled = !busy && !running,
            colors = DsTextFieldDefaults.colors(),
            modifier = Modifier.fillMaxWidth()
        )
        if (snapshots.isNotEmpty()) {
            Column {
                // Newest first: that is the one people go back to.
                snapshots.asReversed().forEach { snap ->
                    key(snap.number) {
                        EntryRow(
                            text = snap.label.ifEmpty { "#${snap.number}" },
                            supporting = stringResource(R.string.snapshot_supporting, snap.time, snap.sizeMb),
                            onDelete = { if (!busy && !running) pending = SnapshotAction.DELETE to snap },
                            deleteDescription = stringResource(R.string.snapshot_delete),
                            trailingBadge = {
                                IconButton(
                                    onClick = { pending = SnapshotAction.RESTORE to snap },
                                    enabled = !busy && !running
                                ) {
                                    Icon(
                                        Icons.Default.Restore,
                                        contentDescription = stringResource(R.string.restore),
                                        tint = MaterialTheme.colorScheme.primary
                                    )
                                }
                            }
                        )
                    }
                }
            }
        }
    }

    pending?.let { (action, snap) ->
        val restore = action == SnapshotAction.RESTORE
        DsDialog(
            onDismiss = { pending = null },
            borderColor = MaterialTheme.colorScheme.error.copy(alpha = 0.3f),
            footer = {
                DialogFooterRow(
                    dismissLabel = stringResource(R.string.cancel),
                    confirmLabel = stringResource(if (restore) R.string.restore else R.string.snapshot_delete),
                    onDismiss = { pending = null },
                    onConfirm = {
                        pending = null
                        run(if (restore) "rollback ${snap.number}" else "snapshot-delete ${snap.number}")
                    },
                    destructive = true
                )
            }
        ) {
            Text(
                text = snap.label.ifEmpty { "#${snap.number}" },
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = stringResource(if (restore) R.string.snapshot_restore_body else R.string.snapshot_delete_body),
                style = MaterialTheme.typography.bodyMedium
            )
        }
    }
}
