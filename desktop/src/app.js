// Velora Studio App Shell, Part Editor for FM & VA, Backup & Restore (Stories 9.5, 9.16, 9.17, 12.13)

const CATALOGS = {
  en: {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "No Velora connected",
    "studio.conn.connecting": "Connecting to your Velora…",
    "studio.conn.unpaired": "Velora found: pairing required",
    "studio.conn.version_mismatch": "Velora version not compatible",
    "studio.conn.paired": "%s connected (the Live Sets shown are simulated for now)",
    "studio.write.simulated": "Writing is disabled: the Live Sets shown are simulated until Studio reads them from the Velora.",
    "studio.pair.ok": "Pair",
    "studio.pair.cancel": "Cancel",
    "studio.pair.failed": "Pairing failed: %s",
    "studio.offline.hint_connecting": "Looking for your Velora…",
    "studio.offline.hint_unpaired": "Enter the pairing code shown on your Velora to connect this computer.",
    "studio.offline.hint_mismatch": "This Velora and Velora Studio do not speak a compatible protocol version. Update the app or the Velora.",
    "studio.offline.hint_unreachable": "Your Velora is not reachable. Check the USB cable; Studio keeps trying.",
    "studio.pair.prompt": "Enter the pairing code shown on your Velora:",
    "studio.title_bar.btn_import": "Import",
    "studio.title_bar.btn_backup": "Backup",
    "studio.title_bar.btn_write": "Write to Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "LIBRARY",
    "studio.sidebar.device": "DEVICE",
    "studio.sidebar.empty_setlists": "No setlists offline",
    "studio.sidebar.empty_library": "Library unavailable offline",
    "studio.sidebar.storage": "Storage",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Backups",
    "studio.sidebar.settings": "Settings",
    "studio.offline.title": "Organize sounds and manage your Velora 73",
    "studio.offline.body": "Connect your Velora via USB to sync setlists, live sets, and library, or manage backups and local settings.",
    "studio.offline.hint": "No device detected on USB ports.",
    "studio.footer.no_backup_yet": "No backup yet",
    "studio.footer.last_backup": "Last backup: %s",
    "studio.settings.title": "Studio Settings",
    "studio.settings.theme": "Theme",
    "studio.settings.theme_system": "System (auto)",
    "studio.settings.theme_stage": "Stage (dark)",
    "studio.settings.theme_studio": "Studio (light)",
    "studio.settings.language": "Language",
    "studio.settings.lang_system": "System language",
    "studio.settings.lang_en": "English",
    "studio.settings.lang_es": "Español",
    "studio.settings.lang_pt_br": "Português (Brasil)",
    "studio.settings.close": "Close",
    "studio.backup.success": "Backup completed successfully.",
    "studio.backup.unsaved_warning": "Unsaved edits on the Velora are not in this backup",
    "studio.backup.needs_newer_studio": "This backup needs a newer Velora Studio",
    "studio.restore.title": "Restore from Backup",
    "studio.restore.merge_label": "Merge",
    "studio.restore.merge_desc": "Add and update from the backup; keep what the backup doesn't have",
    "studio.restore.replace_label": "Replace",
    "studio.restore.replace_desc": "Velora content becomes the backup",
    "studio.restore.counts_summary": "%d setlists to add, %d to update, %d to remove · %d Live Sets to add, %d to update, %d to remove",
    "studio.restore.replace_warning": "Replaces all Live Sets and setlists on the connected Velora with the contents of this backup. Any Live Sets not in this backup will be removed.",
    "studio.restore.btn_confirm": "Restore",
    "studio.restore.btn_cancel": "Cancel",
    "studio.restore.success": "Restore completed successfully.",
    "studio.offline.connect_reason": "Connect a Velora",
    "studio.offline.backup_loaded": "Loaded backup: %s",
    "studio.offline.save_backup": "Save Backup",
    "studio.offline.backup_saved": "Backup saved successfully.",
    "studio.offline.open_backup": "Open Backup (.velorabackup)",
    "studio.part_editor.sound": "Sound",
    "studio.part_editor.sound_groups": "Sound groups",
    "studio.part_editor.brightness_label": "Brightness · KNOB 2",
    "studio.part_editor.brightness_fm_note": "→ modulator levels %s",
    "studio.part_editor.brightness_va_note": "→ filter cutoff",
    "studio.part_editor.edit_envelope": "Edit envelope",
    "studio.part_editor.firmware_mismatch_tag": "! Needs update %s",
    "studio.part_editor.firmware_mismatch_title": "This timbre needs a newer firmware",
    "studio.part_editor.firmware_mismatch_desc": "Velora 73 has firmware %s, which does not include this sound engine. The part stays silent on the Velora and nothing replaces it. Its settings stay in the Live Set.",
    "studio.part_editor.btn_change_timbre": "Change timbre",
    "studio.part_editor.btn_update_firmware": "Update firmware",
    "studio.part_editor.live_edit_on": "LIVE EDIT ON",
    "studio.part_editor.live_edit_edited": "STUDIO · ◆ EDITED",
    "studio.part_editor.live_edit_clean": "STUDIO",
    "studio.part_editor.streaming_ok": "STREAMING OK",
    "studio.part_editor.amp_eq": "Amp envelope + EQ",
    "studio.part_editor.effects": "Effects inserts + sends",
    "studio.part_editor.zone_arp_assign": "Zone & keyboard · Arpeggiator · Assign",
    "studio.part_editor.add_insert": "+ Add insert",
    "studio.part_editor.written_to_velora": "Changes saved to Velora"
  },
  es: {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "No hay ningún Velora conectado",
    "studio.conn.connecting": "Conectando con su Velora…",
    "studio.conn.unpaired": "Velora encontrado: se requiere emparejamiento",
    "studio.conn.version_mismatch": "Versión de Velora no compatible",
    "studio.conn.paired": "%s conectado (los Live Sets mostrados son simulados por ahora)",
    "studio.write.simulated": "Escribir está desactivado: los Live Sets mostrados son simulados hasta que Studio los lea del Velora.",
    "studio.pair.ok": "Emparejar",
    "studio.pair.cancel": "Cancelar",
    "studio.pair.failed": "Error de emparejamiento: %s",
    "studio.offline.hint_connecting": "Buscando su Velora…",
    "studio.offline.hint_unpaired": "Introduzca el código de emparejamiento que muestra su Velora para conectar este ordenador.",
    "studio.offline.hint_mismatch": "Este Velora y Velora Studio no hablan una versión de protocolo compatible. Actualice la aplicación o el Velora.",
    "studio.offline.hint_unreachable": "No se puede alcanzar su Velora. Compruebe el cable USB; Studio sigue intentándolo.",
    "studio.pair.prompt": "Introduzca el código de emparejamiento que muestra su Velora:",
    "studio.title_bar.btn_import": "Importar",
    "studio.title_bar.btn_backup": "Copia de seguridad",
    "studio.title_bar.btn_write": "Escribir en Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "BIBLIOTECA",
    "studio.sidebar.device": "DISPOSITIVO",
    "studio.sidebar.empty_setlists": "Sin setlists sin conexión",
    "studio.sidebar.empty_library": "Biblioteca no disponible sin conexión",
    "studio.sidebar.storage": "Almacenamiento",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Copias de seguridad",
    "studio.sidebar.settings": "Configuración",
    "studio.offline.title": "Organice sonidos y gestione su Velora 73",
    "studio.offline.body": "Conecte su Velora por USB para sincronizar setlists, live sets y biblioteca, o gestionar copias de seguridad y configuraciones locales.",
    "studio.offline.hint": "No se detectó ningún dispositivo en los puertos USB.",
    "studio.footer.no_backup_yet": "Aún no hay copias de seguridad",
    "studio.footer.last_backup": "Última copia de seguridad: %s",
    "studio.settings.title": "Configuración de Studio",
    "studio.settings.theme": "Tema",
    "studio.settings.theme_system": "Sistema (automático)",
    "studio.settings.theme_stage": "Stage (oscuro)",
    "studio.settings.theme_studio": "Studio (claro)",
    "studio.settings.language": "Idioma",
    "studio.settings.lang_system": "Idioma del sistema",
    "studio.settings.lang_en": "Inglés",
    "studio.settings.lang_es": "Español",
    "studio.settings.lang_pt_br": "Portugués (Brasil)",
    "studio.settings.close": "Cerrar",
    "studio.backup.success": "Copia de seguridad completada con éxito.",
    "studio.backup.unsaved_warning": "Los cambios no guardados en el Velora no están en esta copia de seguridad",
    "studio.backup.needs_newer_studio": "Esta copia de seguridad requiere una versión más reciente de Velora Studio",
    "studio.restore.title": "Restaurar desde copia de seguridad",
    "studio.restore.merge_label": "Combinar (Merge)",
    "studio.restore.merge_desc": "Añade y actualiza desde la copia; mantiene lo que no está en la copia",
    "studio.restore.replace_label": "Reemplazar (Replace)",
    "studio.restore.replace_desc": "El contenido de Velora se sustituye por la copia",
    "studio.restore.counts_summary": "%d setlists a añadir, %d a actualizar, %d a eliminar · %d Live Sets a añadir, %d a actualizar, %d a eliminar",
    "studio.restore.replace_warning": "Reemplaza todos los Live Sets y setlists en el Velora conectado con el contenido de esta copia. Se eliminarán los Live Sets que no estén en la copia.",
    "studio.restore.btn_confirm": "Restaurar",
    "studio.restore.btn_cancel": "Cancelar",
    "studio.restore.success": "Restauración completada con éxito.",
    "studio.offline.connect_reason": "Conecte un Velora",
    "studio.offline.backup_loaded": "Copia de seguridad cargada: %s",
    "studio.offline.save_backup": "Guardar copia de seguridad",
    "studio.offline.backup_saved": "Copia de seguridad guardada con éxito.",
    "studio.offline.open_backup": "Abrir copia (.velorabackup)",
    "studio.part_editor.sound": "Sonido",
    "studio.part_editor.sound_groups": "Grupos de sonido",
    "studio.part_editor.brightness_label": "Brillo · KNOB 2",
    "studio.part_editor.brightness_fm_note": "→ niveles de moduladores %s",
    "studio.part_editor.brightness_va_note": "→ corte de filtro",
    "studio.part_editor.edit_envelope": "Editar envolvente",
    "studio.part_editor.firmware_mismatch_tag": "! Requiere actualización %s",
    "studio.part_editor.firmware_mismatch_title": "Este timbre requiere un firmware más reciente",
    "studio.part_editor.firmware_mismatch_desc": "Velora 73 tiene firmware %s, que no incluye este motor de sonido. La parte permanece en silencio en el Velora y nada la sustituye. Sus ajustes permanecen en el Live Set.",
    "studio.part_editor.btn_change_timbre": "Cambiar timbre",
    "studio.part_editor.btn_update_firmware": "Actualizar firmware",
    "studio.part_editor.live_edit_on": "EDICIÓN EN VIVO ACTIVADA",
    "studio.part_editor.live_edit_edited": "STUDIO · ◆ EDITADO",
    "studio.part_editor.live_edit_clean": "STUDIO",
    "studio.part_editor.streaming_ok": "STREAMING OK",
    "studio.part_editor.amp_eq": "Envolvente de amplificador + EQ",
    "studio.part_editor.effects": "Inserciones de efectos + envíos",
    "studio.part_editor.zone_arp_assign": "Zona y teclado · Arpegiador · Asignar",
    "studio.part_editor.add_insert": "+ Añadir inserción",
    "studio.part_editor.written_to_velora": "Cambios guardados en Velora"
  },
  "pt-BR": {
    "studio.title_bar.app_name": "Velora Studio",
    "studio.conn.no_device": "Nenhum Velora conectado",
    "studio.conn.connecting": "Conectando ao seu Velora…",
    "studio.conn.unpaired": "Velora encontrado: pareamento necessário",
    "studio.conn.version_mismatch": "Versão do Velora não compatível",
    "studio.conn.paired": "%s conectado (os Live Sets exibidos são simulados por enquanto)",
    "studio.write.simulated": "A escrita está desativada: os Live Sets exibidos são simulados até o Studio lê-los do Velora.",
    "studio.pair.ok": "Parear",
    "studio.pair.cancel": "Cancelar",
    "studio.pair.failed": "Falha no pareamento: %s",
    "studio.offline.hint_connecting": "Procurando seu Velora…",
    "studio.offline.hint_unpaired": "Digite o código de pareamento exibido no seu Velora para conectar este computador.",
    "studio.offline.hint_mismatch": "Este Velora e o Velora Studio não falam uma versão de protocolo compatível. Atualize o app ou o Velora.",
    "studio.offline.hint_unreachable": "Seu Velora não está acessível. Verifique o cabo USB; o Studio continua tentando.",
    "studio.pair.prompt": "Digite o código de pareamento exibido no seu Velora:",
    "studio.title_bar.btn_import": "Importar",
    "studio.title_bar.btn_backup": "Backup",
    "studio.title_bar.btn_write": "Gravar no Velora",
    "studio.sidebar.setlists": "SETLISTS",
    "studio.sidebar.library": "BIBLIOTECA",
    "studio.sidebar.device": "DISPOSITIVO",
    "studio.sidebar.empty_setlists": "Nenhum setlist offline",
    "studio.sidebar.empty_library": "Biblioteca indisponível offline",
    "studio.sidebar.storage": "Armazenamento",
    "studio.sidebar.firmware": "Firmware",
    "studio.sidebar.backups": "Backups",
    "studio.sidebar.settings": "Configurações",
    "studio.offline.title": "Organize timbres e gerencie seu Velora 73",
    "studio.offline.body": "Conecte seu Velora via USB para sincronizar setlists, live sets e biblioteca, ou gerenciar backups e configurações locais.",
    "studio.offline.hint": "Nenhum dispositivo detectado nas portas USB.",
    "studio.footer.no_backup_yet": "Nenhum backup ainda",
    "studio.footer.last_backup": "Último backup: %s",
    "studio.settings.title": "Configurações do Studio",
    "studio.settings.theme": "Tema",
    "studio.settings.theme_system": "Sistema (automático)",
    "studio.settings.theme_stage": "Stage (escuro)",
    "studio.settings.theme_studio": "Studio (claro)",
    "studio.settings.language": "Idioma",
    "studio.settings.lang_system": "Idioma do sistema",
    "studio.settings.lang_en": "Inglês",
    "studio.settings.lang_es": "Espanhol",
    "studio.settings.lang_pt_br": "Português (Brasil)",
    "studio.settings.close": "Fechar",
    "studio.backup.success": "Backup concluído com sucesso.",
    "studio.backup.unsaved_warning": "Edições não salvas no Velora não estão neste backup",
    "studio.backup.needs_newer_studio": "Este backup precisa de uma versão mais nova do Velora Studio",
    "studio.restore.title": "Restaurar a partir de Backup",
    "studio.restore.merge_label": "Mesclar (Merge)",
    "studio.restore.merge_desc": "Adiciona e atualiza do backup; mantém o que não está no backup",
    "studio.restore.replace_label": "Substituir (Replace)",
    "studio.restore.replace_desc": "O conteúdo do Velora passa a ser exatamente o backup",
    "studio.restore.counts_summary": "%d setlists a adicionar, %d a atualizar, %d a remover · %d Live Sets a adicionar, %d a atualizar, %d a remover",
    "studio.restore.replace_warning": "Substitui todos os Live Sets e setlists no Velora conectado pelo conteúdo deste backup. Quaisquer Live Sets ausentes no backup serão removidos.",
    "studio.restore.btn_confirm": "Restaurar",
    "studio.restore.btn_cancel": "Cancelar",
    "studio.restore.success": "Restauração concluída com sucesso.",
    "studio.offline.connect_reason": "Conecte um Velora",
    "studio.offline.backup_loaded": "Backup carregado: %s",
    "studio.offline.save_backup": "Salvar Backup",
    "studio.offline.backup_saved": "Backup salvo com sucesso.",
    "studio.offline.open_backup": "Abrir Backup (.velorabackup)",
    "studio.part_editor.sound": "Som",
    "studio.part_editor.sound_groups": "Grupos de som",
    "studio.part_editor.brightness_label": "Brilho · KNOB 2",
    "studio.part_editor.brightness_fm_note": "→ níveis de moduladores %s",
    "studio.part_editor.brightness_va_note": "→ corte de filtro",
    "studio.part_editor.edit_envelope": "Editar envelope",
    "studio.part_editor.firmware_mismatch_tag": "! Precisa de atualização %s",
    "studio.part_editor.firmware_mismatch_title": "Este timbre precisa de um firmware mais novo",
    "studio.part_editor.firmware_mismatch_desc": "O Velora 73 possui o firmware %s, que não inclui este mecanismo de som. A parte permanece em silêncio no Velora e nada a substitui. As configurações permanecem no Live Set.",
    "studio.part_editor.btn_change_timbre": "Mudar timbre",
    "studio.part_editor.btn_update_firmware": "Atualizar firmware",
    "studio.part_editor.live_edit_on": "EDIÇÃO AO VIVO ATIVA",
    "studio.part_editor.live_edit_edited": "STUDIO · ◆ EDITADO",
    "studio.part_editor.live_edit_clean": "STUDIO",
    "studio.part_editor.streaming_ok": "STREAMING OK",
    "studio.part_editor.amp_eq": "Envelope de amplificador + EQ",
    "studio.part_editor.effects": "Inserts de efeitos + envios",
    "studio.part_editor.zone_arp_assign": "Zona e teclado · Arpegiador · Atribuir",
    "studio.part_editor.add_insert": "+ Adicionar insert",
    "studio.part_editor.written_to_velora": "Alterações gravadas no Velora"
  }
};

// FM 32 Algorithms metadata per Yamaha/DX standard & engine/core/fm_tables.hpp
const FM_ALGORITHMS = [
  null,
  { id: 1, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 1: OP 2 to OP 1, OP 6 to OP 5 to OP 4 to OP 3, feedback on OP 6" },
  { id: 2, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 2: OP 2 to OP 1, OP 6 to OP 5 to OP 4 to OP 3, feedback on OP 2" },
  { id: 3, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 6, desc: "Algorithm 3: OP 3 to OP 2 to OP 1, OP 6 to OP 5 to OP 4, feedback on OP 6" },
  { id: 4, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 4, desc: "Algorithm 4: OP 3 to OP 2 to OP 1, OP 6 to OP 5 to OP 4, feedback on OP 4" },
  { id: 5, carriers: [1, 3, 5], modulators: [2, 4, 6], feedbackOp: 6, desc: "Algorithm 5: OP 2 to OP 1, OP 4 to OP 3, OP 6 to OP 5, feedback on OP 6" },
  { id: 6, carriers: [1, 3, 5], modulators: [2, 4, 6], feedbackOp: 5, desc: "Algorithm 6: OP 2 to OP 1, OP 4 to OP 3, OP 6 to OP 5, feedback on OP 5" },
  { id: 7, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 7: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 6" },
  { id: 8, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 4, desc: "Algorithm 8: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 4" },
  { id: 9, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 9: OP 2 to OP 1, OP 6 to OP 5 to OP 3, OP 4 to OP 3, feedback on OP 2" },
  { id: 10, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 3, desc: "Algorithm 10: OP 3 to OP 2 to OP 1, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 11, carriers: [1, 4], modulators: [2, 3, 5, 6], feedbackOp: 6, desc: "Algorithm 11: OP 3 to OP 2 to OP 1, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 6" },
  { id: 12, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 12: OP 2 to OP 1, OP 4 to OP 3, OP 5 to OP 3, OP 6 to OP 3, feedback on OP 2" },
  { id: 13, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 13: OP 2 to OP 1, OP 4 to OP 3, OP 5 to OP 3, OP 6 to OP 3, feedback on OP 6" },
  { id: 14, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 14: OP 2 to OP 1, OP 5 to OP 4 to OP 3, OP 6 to OP 4, feedback on OP 6" },
  { id: 15, carriers: [1, 3], modulators: [2, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 15: OP 2 to OP 1, OP 5 to OP 4 to OP 3, OP 6 to OP 4, feedback on OP 2" },
  { id: 16, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 6, desc: "Algorithm 16: OP 2 to OP 1, OP 4 to OP 3 to OP 1, OP 6 to OP 5 to OP 1, feedback on OP 6" },
  { id: 17, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 2, desc: "Algorithm 17: OP 2 to OP 1, OP 4 to OP 3 to OP 1, OP 6 to OP 5 to OP 1, feedback on OP 2" },
  { id: 18, carriers: [1], modulators: [2, 3, 4, 5, 6], feedbackOp: 3, desc: "Algorithm 18: OP 2 to OP 1, OP 3 to OP 1, OP 6 to OP 5 to OP 4 to OP 1, feedback on OP 3" },
  { id: 19, carriers: [1, 4, 5], modulators: [2, 3, 6], feedbackOp: 6, desc: "Algorithm 19: OP 3 to OP 2 to OP 1, OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 20, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 3, desc: "Algorithm 20: OP 3 to OP 1 and OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 21, carriers: [1, 2, 4, 5], modulators: [3, 6], feedbackOp: 3, desc: "Algorithm 21: OP 3 to OP 1 and OP 2, OP 6 to OP 4 and OP 5, feedback on OP 3" },
  { id: 22, carriers: [1, 3, 4, 5], modulators: [2, 6], feedbackOp: 6, desc: "Algorithm 22: OP 2 to OP 1, OP 6 to OP 3, OP 4, OP 5, feedback on OP 6" },
  { id: 23, carriers: [1, 2, 4, 5], modulators: [3, 6], feedbackOp: 6, desc: "Algorithm 23: OP 3 to OP 2, OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 24, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 24: OP 6 to OP 3, OP 4, OP 5, feedback on OP 6" },
  { id: 25, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 25: OP 6 to OP 4 and OP 5, feedback on OP 6" },
  { id: 26, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 6, desc: "Algorithm 26: OP 3 to OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 6" },
  { id: 27, carriers: [1, 2, 4], modulators: [3, 5, 6], feedbackOp: 3, desc: "Algorithm 27: OP 3 to OP 2, OP 5 to OP 4, OP 6 to OP 4, feedback on OP 3" },
  { id: 28, carriers: [1, 3, 6], modulators: [2, 4, 5], feedbackOp: 5, desc: "Algorithm 28: OP 2 to OP 1, OP 5 to OP 4 to OP 3, feedback on OP 5" },
  { id: 29, carriers: [1, 2, 3, 5], modulators: [4, 6], feedbackOp: 6, desc: "Algorithm 29: OP 4 to OP 3, OP 6 to OP 5, feedback on OP 6" },
  { id: 30, carriers: [1, 2, 3, 6], modulators: [4, 5], feedbackOp: 5, desc: "Algorithm 30: OP 5 to OP 4 to OP 3, feedback on OP 5" },
  { id: 31, carriers: [1, 2, 3, 4, 5], modulators: [6], feedbackOp: 6, desc: "Algorithm 31: OP 6 to OP 5, feedback on OP 6" },
  { id: 32, carriers: [1, 2, 3, 4, 5, 6], modulators: [], feedbackOp: 6, desc: "Algorithm 32: all operators to output, feedback on OP 6" }
];

class LocaleManager {
  constructor() {
    this.override = localStorage.getItem("velora_studio_lang") || "system";
  }

  detectOSLocale() {
    const raw = (navigator.language || (navigator.languages && navigator.languages[0]) || "en").toLowerCase();
    if (raw.startsWith("pt")) return "pt-BR";
    if (raw.startsWith("es")) return "es";
    return "en";
  }

  getActiveLocale() {
    if (this.override && this.override !== "system" && CATALOGS[this.override]) {
      return this.override;
    }
    return this.detectOSLocale();
  }

  setOverride(locale) {
    this.override = locale;
    localStorage.setItem("velora_studio_lang", locale);
    this.apply();
  }

  tr(key) {
    const loc = this.getActiveLocale();
    const catalog = CATALOGS[loc] || CATALOGS.en;
    if (catalog[key] !== undefined) return catalog[key];
    if (CATALOGS.en[key] !== undefined) return CATALOGS.en[key];
    return key;
  }

  apply() {
    document.querySelectorAll("[data-i18n]").forEach(el => {
      const key = el.getAttribute("data-i18n");
      const text = this.tr(key);
      if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") {
        el.setAttribute("placeholder", text);
      } else {
        el.textContent = text;
      }
    });
  }
}

class ThemeManager {
  constructor() {
    this.override = localStorage.getItem("velora_studio_theme") || "system";
    this.mediaQuery = window.matchMedia("(prefers-color-scheme: light)");
    this.mediaQuery.addEventListener("change", () => {
      if (this.override === "system") {
        this.apply();
      }
    });
  }

  setOverride(theme) {
    this.override = theme;
    localStorage.setItem("velora_studio_theme", theme);
    this.apply();
  }

  apply() {
    if (this.override === "stage") {
      document.documentElement.setAttribute("data-theme", "stage");
    } else if (this.override === "studio") {
      document.documentElement.setAttribute("data-theme", "studio");
    } else {
      document.documentElement.removeAttribute("data-theme");
    }
  }
}

class FocusManager {
  constructor() {
    this.regions = [
      document.getElementById("sidebar-region"),
      document.getElementById("main-region"),
      document.getElementById("footer-region")
    ].filter(Boolean);

    this.currentRegionIndex = 0;
    this.init();
  }

  init() {
    window.addEventListener("keydown", (e) => {
      if (e.key === "F6") {
        e.preventDefault();
        this.cycleRegion(e.shiftKey ? -1 : 1);
      }
    });
  }

  cycleRegion(direction) {
    if (this.regions.length === 0) return;
    this.currentRegionIndex = (this.currentRegionIndex + direction + this.regions.length) % this.regions.length;
    const target = this.regions[this.currentRegionIndex];
    if (target) {
      target.focus();
    }
  }
}

function announce(message, isWarning = false) {
  const liveRegion = document.getElementById("live-region");
  if (liveRegion) {
    liveRegion.textContent = message;
  }

  const toast = document.getElementById("toast-notice");
  if (toast) {
    toast.textContent = message;
    toast.classList.toggle("warn", isWarning);
    toast.classList.add("active");
    setTimeout(() => {
      toast.classList.remove("active");
    }, 4000);
  }
}

// App Initialization
window.addEventListener("DOMContentLoaded", () => {
  const localeMgr = new LocaleManager();
  const themeMgr = new ThemeManager();
  new FocusManager();

  localeMgr.apply();
  themeMgr.apply();

  // Active state
  let currentDeviceState = null;
  let activeBackup = null;
  let activeBackupPath = "Velora_Backup_Offline.velorabackup";
  let activeLiveSetIndex = 0;
  let selectedPartIndex = 1; // Default to Part 2 (FM) per Story 12.10
  let isConnected = true;
  let deviceLinkActive = false; // true once a real device address is configured: the connection UI then follows the real link
  let deviceHasFmVa = true;
  let deviceFirmwareVersion = "1.4.0";
  let isDirty = false;
  let restoreMode = "merge";
  let restoreCandidateBackup = null;

  // Restore cached last backup time
  const currentDeviceId = "Velora 73";
  const cachedBackupTime = localStorage.getItem("velora_last_backup_" + currentDeviceId);
  if (cachedBackupTime) {
    const backupEl = document.getElementById("footer-backup-status");
    if (backupEl) {
      const tpl = localeMgr.tr("studio.footer.last_backup");
      backupEl.textContent = tpl.replace("%s", cachedBackupTime);
    }
  }

  // Populate FM Algorithm Select Options (1 to 32)
  const algoSelect = document.getElementById("fm-algo-select");
  if (algoSelect) {
    algoSelect.innerHTML = "";
    for (let i = 1; i <= 32; i++) {
      const opt = document.createElement("option");
      opt.value = i;
      opt.textContent = i;
      if (i === 5) opt.selected = true;
      algoSelect.appendChild(opt);
    }
  }

  // Settings Modal Controls
  const modal = document.getElementById("settings-modal");
  const restoreModal = document.getElementById("restore-modal");
  const backdrop = document.getElementById("modal-backdrop");
  const themeSelect = document.getElementById("theme-select");
  const langSelect = document.getElementById("lang-select");

  if (themeSelect) {
    themeSelect.value = themeMgr.override;
    themeSelect.addEventListener("change", (e) => {
      themeMgr.setOverride(e.target.value);
    });
  }

  if (langSelect) {
    langSelect.value = localeMgr.override;
    langSelect.addEventListener("change", (e) => {
      localeMgr.setOverride(e.target.value);
      renderEditor();
    });
  }

  const openSettings = () => {
    if (!modal) return;
    modal.classList.add("active");
    if (backdrop) backdrop.classList.add("active");
    if (themeSelect) themeSelect.focus();
  };

  const closeModals = () => {
    if (modal) modal.classList.remove("active");
    if (restoreModal) restoreModal.classList.remove("active");
    if (backdrop) backdrop.classList.remove("active");
  };

  document.getElementById("btn-open-settings")?.addEventListener("click", openSettings);
  document.getElementById("side-settings")?.addEventListener("click", openSettings);
  document.getElementById("btn-offline-settings")?.addEventListener("click", openSettings);
  document.getElementById("btn-close-settings")?.addEventListener("click", closeModals);
  document.getElementById("btn-close-settings-x")?.addEventListener("click", closeModals);
  document.getElementById("btn-close-restore-x")?.addEventListener("click", closeModals);
  document.getElementById("btn-cancel-restore")?.addEventListener("click", closeModals);
  backdrop?.addEventListener("click", closeModals);

  window.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      closeModals();
    }
  });

  // Mark live edit dirty
  function markLiveEdit(dirty = true) {
    isDirty = dirty;
    const badge = document.getElementById("live-edit-badge");
    const writeBtn = document.getElementById("btn-write");
    const footerStatus = document.getElementById("footer-live-edit-status");

    if (badge) badge.style.display = isDirty ? "inline-block" : "none";
    if (writeBtn && isConnected && deviceLinkActive) {
      writeBtn.disabled = true; // a real unit is connected but the editor still shows simulated Live Sets (until Stories 9.8 / 9.13): never write them
      writeBtn.title = localeMgr.tr("studio.write.simulated");
    } else if (writeBtn && isConnected) {
      writeBtn.disabled = !isDirty;
    }
    if (footerStatus) {
      footerStatus.textContent = isDirty
        ? localeMgr.tr("studio.part_editor.live_edit_edited")
        : localeMgr.tr("studio.part_editor.live_edit_clean");
    }

    // Update active part strip indicator
    const currentStrip = document.querySelector(`.st[data-part-idx="${selectedPartIndex}"]`);
    if (currentStrip) {
      const nameEl = currentStrip.querySelector(".n");
      if (nameEl) {
        const baseName = nameEl.getAttribute("data-basename") || nameEl.textContent.replace(" ◆", "");
        nameEl.setAttribute("data-basename", baseName);
        nameEl.textContent = isDirty ? `${baseName} ◆` : baseName;
      }
    }
  }

  // Draw Dynamic FM Algorithm Diagram SVG (with accessible role="img" & aria-label)
  function renderFmAlgorithmDiagram(algoNum) {
    const meta = FM_ALGORITHMS[algoNum] || FM_ALGORITHMS[5];
    const svg = document.getElementById("fm-algo-svg");
    if (!svg) return;

    svg.setAttribute("aria-label", meta.desc);

    const carriers = new Set(meta.carriers);
    const modulators = new Set(meta.modulators);
    const feedbackOp = meta.feedbackOp;

    // Coordinate layout: 3 columns x 2 rows
    // Col 0: OP 1 & 2; Col 1: OP 3 & 4; Col 2: OP 5 & 6
    const opPos = {
      2: { x: 24, y: 22 },
      1: { x: 24, y: 100 },
      4: { x: 146, y: 22 },
      3: { x: 146, y: 100 },
      6: { x: 268, y: 22 },
      5: { x: 268, y: 100 }
    };

    let elements = "";

    // Operator Boxes (OP 1 to 6)
    for (let op = 1; op <= 6; op++) {
      const pos = opPos[op] || { x: 24 + ((op - 1) % 3) * 122, y: op <= 3 ? 100 : 22 };
      const isCar = carriers.has(op);
      const isMovedByBright = modulators.has(op);
      const strokeDash = isCar ? "" : 'stroke-dasharray="5 3"';
      const brightGlyph = isMovedByBright ? `<text x="${pos.x + 50}" y="${pos.y - 5}" text-anchor="middle" style="fill:var(--accent);font-size:15px">◐</text>` : "";

      elements += `
        <rect x="${pos.x}" y="${pos.y}" width="56" height="38" rx="6" style="fill:var(--raised);stroke:var(--accent)" stroke-width="2" ${strokeDash} />
        <text x="${pos.x + 28}" y="${pos.y + 25}" text-anchor="middle" style="fill:var(--text);font-family:inherit;font-size:16px">OP ${op}</text>
        ${brightGlyph}
      `;
    }

    // Vertical connections between modulators and carriers
    elements += `
      <line x1="52" y1="60" x2="52" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="60" x2="174" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="296" y1="60" x2="296" y2="100" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="52" y1="138" x2="52" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="138" x2="174" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="296" y1="138" x2="296" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="52" y1="176" x2="296" y2="176" style="stroke:var(--text)" stroke-width="2"/>
      <line x1="174" y1="176" x2="174" y2="196" style="stroke:var(--text)" stroke-width="2"/>
      <text x="174" y="216" text-anchor="middle" style="fill:var(--muted);font-size:14px;letter-spacing:2px">OUT</text>
    `;

    // Feedback loop
    const fbPos = opPos[feedbackOp] || { x: 268, y: 22 };
    elements += `
      <path d="M${fbPos.x + 56} ${fbPos.y + 8} h14 v26 h-14" fill="none" style="stroke:var(--text)" stroke-width="2"/>
      <text x="${fbPos.x + 70}" y="${fbPos.y + 54}" text-anchor="end" style="fill:var(--muted);font-size:12px">FB</text>
    `;

    svg.innerHTML = elements;

    // Update Operator Cards titles (Carrier vs Modulator)
    for (let op = 1; op <= 6; op++) {
      const cardTitle = document.getElementById(`op${op}-h`);
      if (cardTitle) {
        const typeStr = carriers.has(op) ? "Carrier" : "Modulator";
        cardTitle.textContent = `OP ${op} · ${typeStr}`;
      }
    }

    // Update Brightness Note text
    const noteEl = document.getElementById("brightness-note-text");
    if (noteEl) {
      if (modulators.size > 0) {
        const modsList = Array.from(modulators).sort().map(m => `OP ${m}`).join(" · ");
        const tpl = localeMgr.tr("studio.part_editor.brightness_fm_note");
        noteEl.textContent = tpl.replace("%s", modsList);
      } else {
        noteEl.textContent = "(no modulators)";
      }
    }

    // Update Feedback Label
    const fbLabel = document.getElementById("fm-feedback-label");
    if (fbLabel) {
      fbLabel.textContent = `Feedback (OP ${feedbackOp})`;
    }
  }

  // Populate Dynamic OP 1 to OP 6 Cards
  function createOperatorCards() {
    const container = document.getElementById("fm-operators-cards-container");
    if (!container || container.children.length > 0) return;

    const defaultRatios = [1.00, 2.00, 1.00, 3.50, 0.50, 7.00];
    const defaultLevels = [99, 78, 99, 62, 90, 41];
    const polylines = [
      "0,55 20,8 80,24 200,55",
      "0,55 10,6 60,40 200,52",
      "0,55 30,10 110,30 200,55",
      "0,55 8,4 40,44 200,54",
      "0,55 20,8 120,20 200,55",
      "0,55 6,10 50,46 200,55"
    ];

    for (let op = 1; op <= 6; op++) {
      const sec = document.createElement("section");
      sec.className = "card";
      sec.id = `op${op}`;
      sec.tabIndex = -1;
      sec.setAttribute("aria-labelledby", `op${op}-h`);

      sec.innerHTML = `
        <h4 id="op${op}-h">OP ${op}</h4>
        <label class="f">
          <span>Ratio</span>
          <input type="number" id="op${op}-ratio" value="${defaultRatios[op - 1].toFixed(2)}" step="0.01" min="0.5" max="32.0" aria-label="OP ${op} Ratio">
        </label>
        <label class="f">
          <span>Level</span>
          <span class="rg">
            <input type="range" id="op${op}-level" min="0" max="99" value="${defaultLevels[op - 1]}" role="slider" aria-label="OP ${op} Level" aria-valuemin="0" aria-valuemax="99" aria-valuenow="${defaultLevels[op - 1]}" aria-valuetext="${defaultLevels[op - 1]} of 99">
            <output id="op${op}-level-out">${defaultLevels[op - 1]}</output>
          </span>
        </label>
        <div class="env">
          <svg viewBox="0 0 200 60" preserveAspectRatio="none" aria-hidden="true">
            <polyline points="${polylines[op - 1]}" fill="none" style="stroke:var(--accent)" stroke-width="2" vector-effect="non-scaling-stroke"/>
          </svg>
        </div>
        <button class="btn" type="button" data-i18n="studio.part_editor.edit_envelope">Edit envelope</button>
      `;

      container.appendChild(sec);
    }
  }

  createOperatorCards();

  // Range Slider ARIA & Input Event Wiring
  function wireRangeInputs() {
    document.querySelectorAll("input[type=range]").forEach((slider) => {
      slider.addEventListener("input", (e) => {
        const val = e.target.value;
        const out = e.target.parentNode.querySelector("output");
        if (out) out.textContent = val;
        e.target.setAttribute("aria-valuenow", val);
        e.target.setAttribute("aria-valuetext", `${val} of ${e.target.max}`);

        markLiveEdit(true);

        // Brightness coordination
        if (slider.id === "va-flt-cutoff") {
          const brightVal = document.getElementById("brightness-val-text");
          if (brightVal) brightVal.textContent = val;
        }

        // Send Tauri live update if connected
        if (window.__TAURI__?.core?.invoke && currentDeviceState) {
          const curLs = currentDeviceState.live_sets?.[activeLiveSetIndex];
          if (curLs) {
            window.__TAURI__.core.invoke("update_part_param", {
              liveSetId: curLs.id,
              partIndex: selectedPartIndex,
              paramId: 1000,
              value: parseFloat(val)
            }).catch(() => {});
          }
        }
      });

      // Keyboard navigation enhancements for ARIA slider
      slider.addEventListener("keydown", (e) => {
        let step = 1;
        if (e.key === "PageUp") {
          step = 10;
        } else if (e.key === "PageDown") {
          step = -10;
        } else if (e.key === "Home") {
          slider.value = slider.min;
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
          return;
        } else if (e.key === "End") {
          slider.value = slider.max;
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
          return;
        }

        if (step !== 1) {
          slider.value = Math.max(Number(slider.min), Math.min(Number(slider.max), Number(slider.value) + step));
          slider.dispatchEvent(new Event("input"));
          e.preventDefault();
        }
      });
    });

    document.querySelectorAll("input[type=number], select, input[type=checkbox]").forEach((inp) => {
      inp.addEventListener("change", () => {
        markLiveEdit(true);
      });
    });

    // Algorithm Select change
    document.getElementById("fm-algo-select")?.addEventListener("change", (e) => {
      const algo = parseInt(e.target.value, 10);
      renderFmAlgorithmDiagram(algo);
      markLiveEdit(true);
    });
  }

  wireRangeInputs();

  // Jump navigation links generator
  function updateJumpNavigation(engineTypeId) {
    const nav = document.getElementById("sound-jump-nav");
    if (!nav) return;
    nav.innerHTML = "";

    let links = [];
    if (engineTypeId === 3) { // FM
      links = [
        { href: "#alg", text: "Algorithm" },
        { href: "#op1", text: "OP 1" },
        { href: "#op2", text: "OP 2" },
        { href: "#op3", text: "OP 3" },
        { href: "#op4", text: "OP 4" },
        { href: "#op5", text: "OP 5" },
        { href: "#op6", text: "OP 6" },
        { href: "#pe", text: "Pitch env" },
        { href: "#lfo", text: "LFO" }
      ];
    } else if (engineTypeId === 4) { // VA
      links = [
        { href: "#osc1", text: "Osc 1" },
        { href: "#osc2", text: "Osc 2" },
        { href: "#mix", text: "Mixer" },
        { href: "#flt", text: "Filter" },
        { href: "#aenv", text: "Amp env" },
        { href: "#fenv", text: "Filter env" },
        { href: "#vlfo", text: "LFO" },
        { href: "#voice", text: "Voice" }
      ];
    }

    links.forEach((l) => {
      const a = document.createElement("a");
      a.href = l.href;
      a.textContent = l.text;
      a.addEventListener("click", (e) => {
        e.preventDefault();
        const targetId = l.href.substring(1);
        const card = document.getElementById(targetId);
        if (card) {
          card.focus();
          card.classList.add("is-focus");
          setTimeout(() => card.classList.remove("is-focus"), 1200);
        }
      });
      nav.appendChild(a);
    });
  }

  // Render Full Studio Part Editor
  function renderEditor() {
    const dataSource = currentDeviceState || activeBackup;
    if (!dataSource || !dataSource.live_sets || dataSource.live_sets.length === 0) {
      document.getElementById("offline-empty-card").style.display = "flex";
      document.getElementById("editor-container").style.display = "none";
      return;
    }

    document.getElementById("offline-empty-card").style.display = "none";
    const editor = document.getElementById("editor-container");
    editor.style.display = "grid";

    // Setlists & Live Sets in sidebar
    const setlistsListEl = document.getElementById("sidebar-setlists-list");
    setlistsListEl.innerHTML = "";
    const setlists = dataSource.setlists?.setlists || [];
    if (setlists.length === 0) {
      const hint = document.createElement("div");
      hint.className = "sidebar-item empty-hint";
      hint.textContent = localeMgr.tr("studio.sidebar.empty_setlists");
      setlistsListEl.appendChild(hint);
    } else {
      setlists.forEach((sl) => {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "sidebar-item";
        item.textContent = sl.name;
        setlistsListEl.appendChild(item);
      });
    }

    const lsListEl = document.getElementById("sidebar-livesets-list");
    lsListEl.innerHTML = "";
    dataSource.live_sets.forEach((ls, idx) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "sidebar-item" + (idx === activeLiveSetIndex ? " on" : "");
      btn.textContent = ls.name;
      btn.addEventListener("click", () => {
        activeLiveSetIndex = idx;
        renderEditor();
      });
      lsListEl.appendChild(btn);
    });

    const curLs = dataSource.live_sets[activeLiveSetIndex] || dataSource.live_sets[0];
    const titleEl = document.getElementById("editor-title");
    titleEl.textContent = curLs.name;

    // Render Part Strips (1-8)
    const stripsNav = document.getElementById("editor-part-strips");
    stripsNav.innerHTML = "";

    const partsList = curLs.parts || [];
    for (let pIdx = 0; pIdx < 8; pIdx++) {
      const p = partsList[pIdx] || {};
      const st = document.createElement("button");
      st.type = "button";
      st.className = "st" + (pIdx === selectedPartIndex ? " sel" : "");
      st.setAttribute("data-part-idx", pIdx);

      const engineTypeId = p.engine?.engine_type_id || (pIdx === 1 ? 3 : pIdx === 3 ? 4 : 1);
      const isFmOrVa = engineTypeId === 3 || engineTypeId === 4;
      const isMismatch = !deviceHasFmVa && isFmOrVa;

      let catName = "SAMPLE";
      let timbreName = "Empty";
      if (pIdx === 0) { catName = "PIANO"; timbreName = "Concert Grand"; }
      else if (pIdx === 1) { catName = "E.PIANO"; timbreName = "Glass Keys"; }
      else if (pIdx === 2) { catName = "PIANO"; timbreName = "Grand Piano"; }
      else if (pIdx === 3) { catName = "SYNTH PAD"; timbreName = "Warm Saw Pad"; }
      else if (pIdx === 4) { catName = "STRINGS"; timbreName = "Strings"; }
      else if (pIdx === 5) { catName = "BASS"; timbreName = "Soft Bass"; }
      else if (pIdx === 6) { catName = "BRASS"; timbreName = "Brass Stab"; }
      else if (pIdx === 7) { catName = "VOICE"; timbreName = "Choir"; }

      const volVal = Math.round((p.volume || 0.7) * 100);

      st.innerHTML = `
        <span class="c">${catName}</span>
        <span class="n" data-basename="${timbreName}">${timbreName}</span>
        ${isMismatch
          ? `<span class="w">! Needs update 1.4</span>`
          : `<span class="m">${volVal}</span>`}
      `;

      st.addEventListener("click", () => {
        selectedPartIndex = pIdx;
        renderEditor();
      });

      stripsNav.appendChild(st);
    }

    // Active Part Details Header
    const activePart = partsList[selectedPartIndex] || {};
    const engineTypeId = activePart.engine?.engine_type_id || (selectedPartIndex === 1 ? 3 : selectedPartIndex === 3 ? 4 : 1);
    const isFmOrVa = engineTypeId === 3 || engineTypeId === 4;
    const isMismatch = !deviceHasFmVa && isFmOrVa;

    let catName = "PIANO";
    let timbreName = "Concert Grand";
    if (selectedPartIndex === 1) { catName = "E.PIANO"; timbreName = "Glass Keys"; }
    else if (selectedPartIndex === 2) { catName = "PIANO"; timbreName = "Grand Piano"; }
    else if (selectedPartIndex === 3) { catName = "SYNTH PAD"; timbreName = "Warm Saw Pad"; }
    else if (selectedPartIndex === 4) { catName = "STRINGS"; timbreName = "Strings"; }
    else if (selectedPartIndex === 5) { catName = "BASS"; timbreName = "Soft Bass"; }
    else if (selectedPartIndex === 6) { catName = "BRASS"; timbreName = "Brass Stab"; }
    else if (selectedPartIndex === 7) { catName = "VOICE"; timbreName = "Choir"; }

    document.getElementById("part-num-label").textContent = `PART ${selectedPartIndex + 1}`;
    document.getElementById("part-name-display").textContent = timbreName;
    document.getElementById("part-category-display").textContent = catName;

    const banner = document.getElementById("firmware-mismatch-banner");
    const fmCards = document.getElementById("fm-sound-cards");
    const vaCards = document.getElementById("va-sound-cards");
    const brightEl = document.getElementById("brightness-readout");

    if (isMismatch) {
      // Version mismatch: hide FM/VA editor cards, show banner (FR99)
      banner.style.display = "grid";
      fmCards.style.display = "none";
      vaCards.style.display = "none";
      brightEl.style.display = "none";
      updateJumpNavigation(0);
    } else {
      banner.style.display = "none";
      brightEl.style.display = "flex";

      if (engineTypeId === 3) { // FM
        fmCards.style.display = "grid";
        vaCards.style.display = "none";
        updateJumpNavigation(3);
        renderFmAlgorithmDiagram(5);
      } else if (engineTypeId === 4) { // VA
        vaCards.style.display = "grid";
        fmCards.style.display = "none";
        updateJumpNavigation(4);
        const noteEl = document.getElementById("brightness-note-text");
        if (noteEl) noteEl.textContent = localeMgr.tr("studio.part_editor.brightness_va_note");
      } else {
        fmCards.style.display = "none";
        vaCards.style.display = "none";
        brightEl.style.display = "none";
        updateJumpNavigation(0);
      }
    }

    markLiveEdit(isDirty);
  }

  // Write to Velora persistence handler
  const writeBtn = document.getElementById("btn-write");
  writeBtn?.addEventListener("click", () => {
    if (window.__TAURI__?.core?.invoke && currentDeviceState) {
      const curLs = currentDeviceState.live_sets?.[activeLiveSetIndex];
      if (curLs) {
        window.__TAURI__.core.invoke("write_to_device", { liveSetId: curLs.id })
          .then(() => {
            markLiveEdit(false);
            announce(localeMgr.tr("studio.part_editor.written_to_velora"));
          })
          .catch((err) => {
            announce("Write failed: " + err, true);
          });
      }
    } else {
      markLiveEdit(false);
      announce(localeMgr.tr("studio.part_editor.written_to_velora"));
    }
  });

  // Mismatch Banner "Update firmware" button (FR99)
  document.getElementById("btn-mismatch-update-fw")?.addEventListener("click", () => {
    deviceHasFmVa = true;
    deviceFirmwareVersion = "1.4.0";
    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("set_firmware_version", { version: "1.4.0" })
        .then(() => {
          renderEditor();
          announce("Firmware updated to 1.4.0. FM and VA sound engines are now available.");
        })
        .catch(() => {
          renderEditor();
        });
    } else {
      renderEditor();
      announce("Firmware updated to 1.4.0. FM and VA sound engines are now available.");
    }
  });

  // Mismatch Banner "Change timbre" button
  document.getElementById("btn-mismatch-change-timbre")?.addEventListener("click", () => {
    // Switch to concert grand
    selectedPartIndex = 0;
    renderEditor();
  });

  // Query Tauri backend for initial connection status and device state
  if (window.__TAURI__?.core?.invoke) {
    window.__TAURI__.core.invoke("get_connection_status").then((conn) => {
      if (conn?.connected && !deviceLinkActive) {
        isConnected = true;
        deviceHasFmVa = conn.has_fm_va;
        deviceFirmwareVersion = conn.firmware_version || "1.4.0";

        const connText = document.querySelector("#connection-status .conn-text");
        if (connText) connText.textContent = conn.device_name || "Velora 73 connected";

        window.__TAURI__.core.invoke("get_current_device_state").then((state) => {
          currentDeviceState = state;
          renderEditor();
        }).catch(() => {
          renderEditor();
        });
      }
    }).catch(() => {
      // Mock defaults for preview/testing
      currentDeviceState = {
        device_model: "Velora 73",
        firmware_version: "1.4.0",
        has_fm_va: true,
        live_sets: [
          {
            id: "ls-perfect",
            name: "Live Set 03 · Perfect",
            parts: [
              { volume: 0.72 },
              { volume: 0.65, engine: { engine_type_id: 3 } },
              { volume: 0.58 },
              { volume: 0.55, engine: { engine_type_id: 4 } },
              { volume: 0.58 },
              { volume: 0.64 },
              { volume: 0.50 },
              { volume: 0.44 }
            ]
          }
        ]
      };
      renderEditor();
    });
  } else {
    // Web preview mode default
    currentDeviceState = {
      device_model: "Velora 73",
      firmware_version: "1.4.0",
      has_fm_va: true,
      live_sets: [
        {
          id: "ls-perfect",
          name: "Live Set 03 · Perfect",
          parts: [
            { volume: 0.72 },
            { volume: 0.65, engine: { engine_type_id: 3 } },
            { volume: 0.58 },
            { volume: 0.55, engine: { engine_type_id: 4 } },
            { volume: 0.58 },
            { volume: 0.64 },
            { volume: 0.50 },
            { volume: 0.44 }
          ]
        }
      ]
    };
    renderEditor();
  }

  // ---- Real device link (Foundation: Velora Studio device connection) ------------------------------------------------------------------------------
  // The Rust side (src-tauri/src/device.rs) connects to the unit through the sysd bridge. Until a device address is configured (device_connect, or the
  // VELORA_DEVICE=host:port environment variable) the simulated device above stays the offline fallback. Once configured, the title-bar indicator and the
  // offline explanation follow the real status: disconnected | connecting | unpaired | paired | version-mismatch. Stories 9.8 / 9.13 read the State store.
  const deviceStore = {
    status: null,
    state: null,
    stateListeners: new Set(),
    statusListeners: new Set(),
  };
  window.veloraDevice = {
    get status() { return deviceStore.status; },
    get state() { return deviceStore.state; },
    onState(cb) { deviceStore.stateListeners.add(cb); return () => deviceStore.stateListeners.delete(cb); },
    onStatus(cb) { deviceStore.statusListeners.add(cb); return () => deviceStore.statusListeners.delete(cb); },
    connect(host, port) { return window.__TAURI__.core.invoke("device_connect", { host, port }); },
    pair(code, name) { return window.__TAURI__.core.invoke("device_pair", { code, name }); },
    disconnect() { return window.__TAURI__.core.invoke("device_disconnect"); },
    // request: a ws.v1 ClientMessage as JSON, e.g. { body: { GetState: {} } }; resolves to the ServerMessage as JSON.
    request(request) { return window.__TAURI__.core.invoke("device_request", { request }); },
  };
  let pairPromptedFor = null;
  function applyDeviceStatus(st) {
    if (!st || !st.configured) return; // no real device configured: the simulation stays
    const wasLinked = deviceLinkActive;
    deviceLinkActive = true;
    deviceStore.status = st;
    const paired = st.status === "paired";
    const connText = document.querySelector("#connection-status .conn-text");
    const footerDev = document.getElementById("footer-device-status");
    const hintEl = document.querySelector("#offline-empty-card .offline-hint");
    let label;
    let hint;
    switch (st.status) {
      case "paired":
        label = localeMgr.tr("studio.conn.paired").replace("%s", st.model || "Velora");
        break;
      case "unpaired":
        label = localeMgr.tr("studio.conn.unpaired");
        hint = localeMgr.tr("studio.offline.hint_unpaired");
        break;
      case "version-mismatch":
        label = localeMgr.tr("studio.conn.version_mismatch");
        hint = localeMgr.tr("studio.offline.hint_mismatch");
        break;
      case "connecting":
        label = localeMgr.tr("studio.conn.connecting");
        hint = localeMgr.tr("studio.offline.hint_connecting");
        break;
      default:
        label = localeMgr.tr("studio.conn.no_device");
        hint = localeMgr.tr("studio.offline.hint_unreachable");
    }
    if (connText) connText.textContent = label;
    if (footerDev) footerDev.textContent = label;
    if (hintEl && hint) hintEl.textContent = hint;
    const connEl = document.getElementById("connection-status");
    if (connEl) connEl.dataset.link = st.status;
    const becameConnected = paired && (!isConnected || !wasLinked);
    isConnected = paired;
    if (paired) {
      if (st.firmware_version) deviceFirmwareVersion = st.firmware_version;
      if (becameConnected && window.__TAURI__?.core?.invoke) {
        // The editor still shows the simulated Live Sets until Stories 9.8 / 9.13 map the real State.
        window.__TAURI__.core.invoke("get_current_device_state").then((state) => { currentDeviceState = state; renderEditor(); }).catch(() => {});
      }
      pairPromptedFor = null;
    } else {
      currentDeviceState = null;
      renderEditor(); // the existing offline card (and any loaded backup)
    }
    deviceStore.statusListeners.forEach((cb) => { try { cb(st); } catch (e) { /* a listener never breaks the link */ } });
    if (paired) markLiveEdit(false); // refreshes the Write button state for the simulated data
    if (st.status !== "unpaired") {
      pairPromptedFor = null;
      closePairDialog();
    } else if (pairPromptedFor !== st.host + ":" + st.port) {
      pairPromptedFor = st.host + ":" + st.port;
      openPairDialog();
    }
  }

  // Placeholder pairing dialog (the real one is Story 9.6): a minimal in-page input (window.prompt is not available in Tauri 2's webviews). A wrong code
  // keeps it open with the reason; Cancel closes it and clicking the connection indicator while "unpaired" opens it again.
  let pairDialogEl = null;
  function closePairDialog() {
    if (pairDialogEl) pairDialogEl.remove();
    pairDialogEl = null;
  }
  function openPairDialog() {
    if (pairDialogEl) return;
    const overlay = document.createElement("div");
    overlay.className = "pair-dialog-backdrop";
    const box = document.createElement("div");
    box.className = "pair-dialog";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-modal", "true");
    const label = document.createElement("label");
    label.textContent = localeMgr.tr("studio.pair.prompt");
    const input = document.createElement("input");
    input.type = "text";
    input.inputMode = "numeric";
    input.autocomplete = "off";
    input.maxLength = 12;
    label.appendChild(input);
    const err = document.createElement("div");
    err.className = "pair-dialog-error";
    err.setAttribute("role", "alert");
    const row = document.createElement("div");
    row.className = "pair-dialog-actions";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "btn";
    cancel.textContent = localeMgr.tr("studio.pair.cancel");
    const ok = document.createElement("button");
    ok.type = "button";
    ok.className = "btn btn-primary";
    ok.textContent = localeMgr.tr("studio.pair.ok");
    row.append(cancel, ok);
    box.append(label, err, row);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    pairDialogEl = overlay;
    input.focus();
    const submit = () => {
      const code = input.value.trim();
      if (!code) return;
      ok.disabled = true;
      const name = (navigator.platform || "Computer").slice(0, 60);
      window.veloraDevice.pair(code, name).then(closePairDialog).catch((e) => {
        ok.disabled = false;
        err.textContent = localeMgr.tr("studio.pair.failed").replace("%s", String(e));
        input.select();
      });
    };
    ok.addEventListener("click", submit);
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") submit();
      if (e.key === "Escape") closePairDialog();
    });
    cancel.addEventListener("click", closePairDialog);
  }
  document.getElementById("connection-status")?.addEventListener("click", () => {
    if (deviceStore.status && deviceStore.status.status === "unpaired") openPairDialog();
  });
  if (window.__TAURI__?.event?.listen && window.__TAURI__?.core?.invoke) {
    window.__TAURI__.event.listen("device://status", (e) => applyDeviceStatus(e.payload));
    window.__TAURI__.event.listen("device://state", (e) => {
      deviceStore.state = e.payload;
      deviceStore.stateListeners.forEach((cb) => { try { cb(e.payload); } catch (err) { /* ignore */ } });
    });
    window.__TAURI__.core.invoke("device_status").then(applyDeviceStatus).catch(() => {});
    window.__TAURI__.core.invoke("device_state").then((st) => { if (st) deviceStore.state = st; }).catch(() => {});
  }

  // Backup and Restore actions
  document.getElementById("btn-backup")?.addEventListener("click", () => {
    const defaultName = `Velora_Backup_${new Date().toISOString().slice(0, 10)}.velorabackup`;
    const targetPath = prompt("Enter backup file location/name:", defaultName);
    if (!targetPath) return;

    if (window.__TAURI__?.core?.invoke) {
      window.__TAURI__.core.invoke("create_backup", { filePath: targetPath })
        .then((b) => {
          const nowStr = new Date().toLocaleString();
          localStorage.setItem("velora_last_backup_" + currentDeviceId, nowStr);
          announce(localeMgr.tr("studio.backup.success"));
        })
        .catch((err) => {
          announce("Backup failed: " + err, true);
        });
    } else {
      announce(localeMgr.tr("studio.backup.success"));
    }
  });

  const restoreFileInput = document.getElementById("restore-file-input");
  document.getElementById("btn-restore")?.addEventListener("click", () => {
    restoreFileInput?.click();
  });
});
