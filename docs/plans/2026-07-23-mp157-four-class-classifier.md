# STM32MP157 Four-Class Classifier Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Quantize the new four-class MobileNetV3-Small model, make the MP157 classifier derive its class count from the ONNX output, and deploy the verified model and binaries without changing the segmentation model.

**Architecture:** Keep the stable board-side classifier filenames while replacing their contents. Validate the class-count contract at the `ONNX output -> labels -> probability aggregation` boundary, then deploy through the existing VM cross-build and board service workflow. Treat the existing UNet model as immutable and compare its SHA256 before and after deployment.

**Tech Stack:** Python, PyTorch export artifacts, ONNX Runtime quantization, C++14 ONNX Runtime API, shell regression tests, ST OpenSTLinux Qt/Wayland SDK, SSH/SCP, STM32MP157 ARMv7.

---

### Task 1: Add A Failing Dynamic-Class Contract Test

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`
- Test: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**

1. Add static assertions requiring `defect_classify.cpp` to read the output tensor shape, derive `model_class_count`, compare it with `labels.size()`, and reject the legacy `MODEL_CLASS_COUNT = 6` declaration.
2. Run `sh ./test_qt_kms_overlay_assets.sh` in the Qt module.
3. Confirm the new assertion fails because the classifier still uses a fixed six-class constant.

### Task 2: Implement Dynamic ONNX Output And Labels Validation

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/defect_classify.cpp`
- Test: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**

1. Change `load_labels()` to parse all non-negative continuous `idx_to_class` entries without preallocating six elements.
2. Reject duplicate indexes, missing index zero, gaps, empty labels, and labels that contain neither the `good` nor `bad` token.
3. After creating the ONNX Runtime session, inspect the single output tensor shape and require rank 2 with a positive final dimension.
4. Store the final dimension as `model_class_count`, require equality with `labels.size()`, and use it for softmax and all probability loops.
5. After inference, verify the returned tensor element count equals `model_class_count` for batch size one before reading logits.
6. Run the static test and `git diff --check`; both must pass.

### Task 3: Quantize And Evaluate The New Four-Class Model

**Files:**
- Input: `D:/model_picture/checkpoints_classify_4classes/defect_classifier_4classes.onnx`
- Input: `D:/model_picture/checkpoints_classify_4classes/defect_classifier_4classes_labels.json`
- Create: `D:/model_picture/checkpoints_classify_4classes/defect_classifier_static_mixed_int8.onnx`
- Create: `D:/model_picture/checkpoints_classify_4classes/defect_classifier_static_mixed_int8_labels.json`

**Steps:**

1. Run FP32 batch evaluation on `D:/model_picture/datasets_classify/val` and retain the total and per-class results.
2. Run `quantize_classify_int8.py --preset static_mixed` with all 160 validation images as calibration data.
3. Verify ONNX Runtime loads the INT8 model and reports input `N×3×224×224` and output `N×4`.
4. Run INT8 batch evaluation on the same validation set.
5. Compare FP32 and INT8 accuracy. Continue only when the INT8 loss is no more than one percentage point and every class is represented; otherwise reduce the quantized-node whitelist and repeat.
6. Record SHA256 and file sizes for FP32, INT8, and labels.

### Task 4: Update Deployment Contracts And Module Documentation

**Files:**
- Modify: `20_uvc_camera/qt_camera_display/deploy_qt_camera_display.sh`
- Modify: `20_uvc_camera/qt_camera_display/README.md`
- Modify: `20_uvc_camera/qt_camera_display/test_qt_kms_overlay_assets.sh`

**Steps:**

1. Change the deployment script's default classifier source paths to `checkpoints_classify_4classes`, while preserving the board destination filenames.
2. Add regression assertions for the new source directory, dynamic class validation, four labels, and unchanged UNet destination path.
3. Update the README file list, model table, deployment command, validation table, troubleshooting, and change log.
4. Document exact commands for model hash checks, single-image read/inference verification, Qt self-test, service status, and rollback.
5. State explicitly that the classifier now recognizes only flat washers and split washers, while the segmentation model remains the previous model.
6. Run the module static test and `git diff --check`.

### Task 5: Sync To VM And Cross-Build

**Files:**
- VM module: `/home/cfr/linux/Linux_Drivers/20_uvc_camera/qt_camera_display`
- VM model staging: `/home/cfr/linux/model_picture/checkpoints_classify_4classes`

**Steps:**

1. Check VM repository status and back up only the files that will be overwritten.
2. Transfer the changed source files, INT8 ONNX, and labels; compare Windows and VM SHA256.
3. Record the current VM/NFS/board UNet model SHA256 before deployment.
4. Run `sh ./test_qt_kms_overlay_assets.sh` in the VM module.
5. Source `/opt/st/stm32mp1/3.1-snapshot/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi`.
6. Build `defect-classify` and `qt_camera_display`; verify both outputs are ARM 32-bit executables.
7. Run a VM-hosted classifier smoke test when the ARM runtime environment permits; otherwise reserve execution for the board.

### Task 6: Deploy To STM32MP157 Without Replacing UNet

**Files:**
- Board: `/root/qt_camera_display/defect-classify`
- Board: `/root/qt_camera_display/qt_camera_display`
- Board: `/root/qt_camera_display/models/defect_classifier_static_mixed_int8.onnx`
- Board: `/root/qt_camera_display/models/defect_classifier_static_mixed_int8_labels.json`
- Immutable: `/root/qt_camera_display/models/defect_unet_test_decoder_head_int8.onnx`

**Steps:**

1. Back up the current board classifier binary, Qt binary, classification ONNX, and labels with a timestamped directory.
2. Deploy the new classifier model, labels, classifier binary, and Qt binary; do not copy a new UNet model.
3. Set executable/file permissions, atomically replace `.new` files, and run `sync`.
4. Restart `/root/qt_camera_display/run_qt_kms_overlay_display.sh` and inspect status/log output.
5. Compare VM and board SHA256 for every replaced artifact.
6. Compare the post-deployment UNet SHA256 with the pre-deployment value; restore immediately if it differs.

### Task 7: Board Acceptance And Repository Completion

**Files:**
- Modify if results require documentation updates: `20_uvc_camera/qt_camera_display/README.md`

**Steps:**

1. Select one known image for each of the four classes and run `defect-classify` directly on the board.
2. Confirm outputs contain only `splitwasher_bad`, `splitwasher_good`, `washer_bad`, or `washer_good`, and that `status` agrees with the suffix.
3. Run `./qt_camera_display --detect-self-test` and verify classification, unchanged segmentation inference, fused result, history, and upload behavior.
4. Check service logs for ONNX shape/labels mismatch errors and confirm none are present.
5. Run final `git diff --check`, module static tests, and focused status checks.
6. Commit only the task-related Windows files; leave `11111/` and `11111.zip` untouched. Do not push the Windows repository.
7. In the VM `/home/cfr/linux/Linux_Drivers` repository, precisely add only this task's files, commit, and push according to the project VM repository policy.
