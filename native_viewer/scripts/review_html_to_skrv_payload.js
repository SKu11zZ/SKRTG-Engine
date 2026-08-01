"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const POSE_KEYS = ["sourceTrs", "fkTrs", "foundationTrs", "finalTrs"];
const MESH_BINARY_KEYS = ["p", "tri", "io", "ic", "iw", "cb", "co"];

function invariant(condition, message) {
  if (!condition) throw new Error(message);
}

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex").toUpperCase();
}

function loadViewerData(viewerPath) {
  const html = fs.readFileSync(viewerPath, "utf8");
  const beginMarker = "<script>const DATA=";
  const endMarker = ";const $=";
  const begin = html.indexOf(beginMarker);
  const dataStart = begin + beginMarker.length;
  const dataEnd = html.indexOf(endMarker, dataStart);
  invariant(begin >= 0 && dataEnd > dataStart, "viewer DATA boundaries are invalid");
  const dataText = html.slice(dataStart, dataEnd);
  return { data: JSON.parse(dataText), dataText };
}

function portableBasename(value) {
  return String(value).replace(/\\/g, "/").split("/").pop();
}

function portableFileName(value, label) {
  const fileName = String(value);
  invariant(fileName.length > 0, `${label} is empty`);
  invariant(fileName === portableBasename(fileName),
    `${label} must be a file name, not a path`);
  invariant(!/[\\/:<>"|?*]/.test(fileName),
    `${label} contains a non-portable character`);
  invariant(!/[. ]$/.test(fileName),
    `${label} has a trailing dot or space`);
  for (const character of fileName) {
    const code = character.codePointAt(0);
    invariant(code >= 0x20 && code <= 0x7e,
      `${label} is outside the portable ASCII subset`);
  }
  const stem = fileName.split(".", 1)[0].toLowerCase();
  invariant(!/^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/.test(stem),
    `${label} uses a reserved Windows device name`);
  return fileName;
}

function decodeCanonicalBase64(value, label) {
  invariant(typeof value === "string", `${label} is not base64 text`);
  const bytes = Buffer.from(value, "base64");
  invariant(bytes.toString("base64") === value, `${label} is not canonical base64`);
  return bytes;
}

function createBlobStore(payloadDirectory) {
  const blobs = new Map();
  let logicalBytes = 0;

  function add(base64, scalarType, expectedElements, label) {
    const bytes = decodeCanonicalBase64(base64, label);
    invariant(bytes.byteLength % 4 === 0, `${label} is not 32-bit aligned`);
    const elementCount = bytes.byteLength / 4;
    invariant(
      elementCount === expectedElements,
      `${label} element count ${elementCount} != ${expectedElements}`,
    );
    const digest = sha256(bytes);
    const relativePath = `blobs/${digest}.bin`;
    const existing = blobs.get(digest);
    if (existing) {
      invariant(existing.equals(bytes), `${label} has a SHA-256 collision`);
    } else {
      fs.writeFileSync(path.join(payloadDirectory, relativePath), bytes);
      blobs.set(digest, bytes);
    }
    logicalBytes += bytes.byteLength;
    return {
      path: relativePath,
      scalarType,
      byteOrder: "little_endian",
      elementCount,
      byteCount: bytes.byteLength,
      sha256: digest,
    };
  }

  return {
    add,
    summary: () => ({
      uniqueBlobCount: blobs.size,
      uniqueBlobBytes: [...blobs.values()].reduce(
        (sum, bytes) => sum + bytes.byteLength,
        0,
      ),
      logicalBlobBytes: logicalBytes,
    }),
  };
}

function convertMesh(mesh, blobStore, label) {
  invariant(mesh.p && mesh.tri && mesh.io && mesh.ic && mesh.iw && mesh.cb && mesh.co,
    `${label} binary fields are incomplete`);
  invariant(Array.isArray(mesh.fallback) && mesh.fallback.length === 12,
    `${label} fallback affine must contain 12 float values`);
  const influenceBytes = decodeCanonicalBase64(mesh.ic, `${label}.ic`);
  const weightBytes = decodeCanonicalBase64(mesh.iw, `${label}.iw`);
  invariant(influenceBytes.byteLength === weightBytes.byteLength,
    `${label} influence index/weight arrays differ in length`);
  const influenceCount = influenceBytes.byteLength / 4;
  const offsetBytes = decodeCanonicalBase64(mesh.io, `${label}.io`);
  invariant(offsetBytes.byteLength === (mesh.controlPointCount + 1) * 4,
    `${label} influence offsets do not match control points`);
  invariant(offsetBytes.readUInt32LE(offsetBytes.byteLength - 4) === influenceCount,
    `${label} final influence offset does not match influence count`);

  const metadata = { ...mesh };
  for (const key of MESH_BINARY_KEYS) delete metadata[key];
  return {
    ...metadata,
    arrays: {
      positions: blobStore.add(
        mesh.p, "float32", mesh.controlPointCount * 3, `${label}.p`,
      ),
      triangleIndices: blobStore.add(
        mesh.tri, "uint32", mesh.triangleCount * 3, `${label}.tri`,
      ),
      influenceOffsets: blobStore.add(
        mesh.io, "uint32", mesh.controlPointCount + 1, `${label}.io`,
      ),
      influenceClusterIndices: blobStore.add(
        mesh.ic, "uint32", influenceCount, `${label}.ic`,
      ),
      influenceWeights: blobStore.add(
        mesh.iw, "float32", influenceCount, `${label}.iw`,
      ),
      clusterBoneIndices: blobStore.add(
        mesh.cb, "uint32", mesh.clusterCount, `${label}.cb`,
      ),
      clusterBindOffsets3x4: blobStore.add(
        mesh.co, "float32", mesh.clusterCount * 12, `${label}.co`,
      ),
    },
  };
}

function convertMeshPackage(meshPackage, blobStore, label) {
  invariant(Array.isArray(meshPackage.meshes), `${label} has no mesh inventory`);
  const metadata = { ...meshPackage };
  delete metadata.meshes;
  return {
    ...metadata,
    meshes: meshPackage.meshes.map((mesh, index) =>
      convertMesh(mesh, blobStore, `${label}.meshes[${index}]`),
    ),
  };
}

function resolveVerifiedExport(entry, reviewDirectory) {
  invariant(entry && typeof entry === "object", "verified export is not an object");
  const fileName = portableFileName(
    portableBasename(entry.path),
    "verified export file name",
  );
  const source = path.join(reviewDirectory, fileName);
  invariant(fs.existsSync(source), `verified export is missing: ${source}`);
  const bytes = fs.readFileSync(source);
  invariant(sha256(bytes) === entry.sha256.toUpperCase(),
    `verified export hash mismatch: ${source}`);
  return { source, bytes };
}

function createPayloadInStaging(viewerPath, verificationPath, payloadDirectory) {
  invariant(!fs.existsSync(payloadDirectory),
    `payload output already exists: ${payloadDirectory}`);
  fs.mkdirSync(path.join(payloadDirectory, "blobs"), { recursive: true });
  fs.mkdirSync(path.join(payloadDirectory, "exports"), { recursive: true });

  const { data, dataText } = loadViewerData(viewerPath);
  const verificationText = fs.readFileSync(verificationPath, "utf8");
  const verification = JSON.parse(verificationText);
  invariant(data.schema === "skrtg.d1_17b_retarget_review_viewer.v3",
    `unsupported source review schema: ${data.schema}`);
  invariant(
    verification.schema === "skrtg.d1_17b_mesh_and_fbx_export_verification.v1",
    `unsupported verification schema: ${verification.schema}`,
  );
  invariant(verification.status === "pass", "source FBX verification is not pass");
  invariant(Array.isArray(data.clips) && data.clips.length > 0, "no review clips found");
  invariant(Array.isArray(data.sourceMeshes) && data.sourceMeshes.length === data.clips.length,
    "one source mesh package per clip is required");
  invariant(data.targetMesh && typeof data.targetMesh === "object",
    "target mesh package is missing");
  invariant(Array.isArray(data.sourceBones) && Array.isArray(data.targetBones),
    "source/target skeleton inventories are missing");
  invariant(Array.isArray(data.retargetChains), "retarget chain inventory is missing");
  invariant(Array.isArray(verification.exports), "verified export inventory is missing");
  invariant(Array.isArray(verification.source_animations),
    "verified source animation inventory is missing");
  invariant(verification.target_tpose && typeof verification.target_tpose === "object",
    "verified target T-pose identity is missing");

  const blobStore = createBlobStore(payloadDirectory);
  const sourceMeshes = data.sourceMeshes.map((meshPackage, index) =>
    convertMeshPackage(meshPackage, blobStore, `sourceMeshes[${index}]`),
  );
  const targetMesh = convertMeshPackage(data.targetMesh, blobStore, "targetMesh");
  const clips = data.clips.map((clip, index) => {
    const converted = { ...clip };
    const expectedElements = {
      sourceTrs: clip.frameCount * data.sourceBones.length * 10,
      fkTrs: clip.frameCount * data.targetBones.length * 10,
      foundationTrs: clip.frameCount * data.targetBones.length * 10,
      finalTrs: clip.frameCount * data.targetBones.length * 10,
    };
    for (const key of POSE_KEYS) {
      converted[key] = blobStore.add(
        clip[key], "float32", expectedElements[key],
        `clips[${index}].${key}`,
      );
      converted[key].layout =
        "frame_major_bone_major_tx_ty_tz_qx_qy_qz_qw_sx_sy_sz";
      converted[key].transformStrideFloat32 = 10;
    }
    invariant(clip.sourceMeshIndex === index,
      `clip ${clip.id} sourceMeshIndex is not stable`);
    return converted;
  });

  const reviewMetadata = { ...data };
  delete reviewMetadata.schema;
  delete reviewMetadata.sourceMeshes;
  delete reviewMetadata.targetMesh;
  delete reviewMetadata.clips;

  const reviewDirectory = path.dirname(viewerPath);
  const exportPaths = new Set();
  const exports = verification.exports.map((entry) => {
    const { source, bytes } = resolveVerifiedExport(entry, reviewDirectory);
    const fileName = portableFileName(
      portableBasename(source),
      "verified export file name",
    );
    const relativePath = `exports/${fileName}`;
    invariant(!exportPaths.has(relativePath.toLowerCase()),
      `duplicate portable export path: ${relativePath}`);
    exportPaths.add(relativePath.toLowerCase());
    fs.copyFileSync(source, path.join(payloadDirectory, relativePath));
    return {
      ...entry,
      path: relativePath,
      byteCount: bytes.byteLength,
      sha256: entry.sha256.toUpperCase(),
    };
  });
  invariant(exports.length === data.clips.length * 2,
    "each clip requires Foundation and Final exports");
  for (const clip of data.clips) {
    for (const [lane, fileName] of [
      ["foundation", clip.foundationExportFbx],
      ["final", clip.exportFbx],
    ]) {
      portableFileName(fileName, `clip ${clip.id} ${lane} export`);
      invariant(exports.some((entry) =>
        entry.clip_id === clip.id && entry.lane === lane &&
        portableBasename(entry.path) === fileName),
      `clip ${clip.id} is missing its ${lane} export`);
    }
  }

  const manifest = {
    schema: "skrtg.skrv.manifest.v1",
    contractVersion: 1,
    sourceReviewSchema: data.schema,
    provenance: {
      sourceReviewDataSha256: sha256(Buffer.from(dataText, "utf8")),
      sourceViewerFile: path.basename(viewerPath),
      sourceVerificationFile: path.basename(verificationPath),
      sourceVerificationSha256: sha256(Buffer.from(verificationText, "utf8")),
      adapter: "skrtg.review_html_to_skrv_payload.v1",
      algorithmRecomputed: false,
      fbxReserialized: false,
    },
    storageContract: {
      scalarEncoding: "IEEE_754_or_uint32",
      byteOrder: "little_endian",
      translationUnit: "centimeter",
      quaternionOrder: "x_y_z_w",
      transformFloat32Layout:
        "tx_ty_tz_qx_qy_qz_qw_sx_sy_sz",
      meshClusterBindOffsetLayout: "row_major_3x4_affine",
      packageForm: "auditable_directory",
    },
    displayContract: {
      columns: [
        "original_source_model_space",
        "fk_plus_anchor_aligned_original_10_percent",
        "foundation_fk_ik_plus_anchor_aligned_original_10_percent",
        "final_result_only",
      ],
      sourceGhostOpacity: 0.1,
      resultOpacity: 1.0,
      synchronized: ["frame", "camera", "projection", "display_scale"],
      originalIndependentPanelKeepsOriginalSpace: true,
      overlaysUseExplicitAnchorAlignedSourceGhost: true,
      nonIkBonesReducedOpacity: true,
      virtualGroundIsDisplayOnly: true,
      goalHistoryIsDisplayOnly: true,
      noGroundOrContactSemanticsClaimed: true,
    },
    counts: {
      clipCount: clips.length,
      frameCount: clips.reduce((sum, clip) => sum + clip.frameCount, 0),
      sourceBoneCount: data.sourceBones.length,
      targetBoneCount: data.targetBones.length,
      mappedChainCount: data.retargetChains.length,
      goalChainCount: data.retargetChains.filter((chain) => chain.ikMode !== "fk_only").length,
    },
    snapshot: {
      ...reviewMetadata,
      sourceMeshes,
      targetMesh,
      clips,
    },
    verifiedExports: exports,
    verificationContract: {
      schema: verification.schema,
      roundtripTolerances: verification.roundtrip_tolerances,
      viewerContract: verification.viewer_contract,
      sourceAnimations: verification.source_animations.map((entry) => ({
        clipId: entry.clip_id,
        fileName: portableBasename(entry.path),
        sha256: entry.sha256,
        unchanged: entry.unchanged,
      })),
      targetTpose: {
        fileName: portableBasename(verification.target_tpose.path),
        sha256: verification.target_tpose.sha256,
        unchanged: verification.target_tpose.unchanged,
      },
    },
  };

  fs.writeFileSync(
    path.join(payloadDirectory, "manifest.json"),
    `${JSON.stringify(manifest, null, 2)}\n`,
  );
  fs.writeFileSync(
    path.join(payloadDirectory, "README.txt"),
    [
      "SKRTG SKRV v1 payload staging directory.",
      "Pack this directory with skrv_pack; do not edit binary blobs or exports.",
      "No retarget algorithm was executed and no FBX was reserialized by this adapter.",
      "",
    ].join("\n"),
  );

  return {
    schema: "skrtg.skrv_payload_export.v1",
    status: "pass",
    payloadDirectory: path.resolve(payloadDirectory),
    sourceReviewDataSha256: manifest.provenance.sourceReviewDataSha256,
    clipCount: manifest.counts.clipCount,
    frameCount: manifest.counts.frameCount,
    sourceBoneCount: manifest.counts.sourceBoneCount,
    targetBoneCount: manifest.counts.targetBoneCount,
    exportCount: exports.length,
    ...blobStore.summary(),
  };
}

function createPayload(viewerPath, verificationPath, payloadDirectory) {
  const finalDirectory = path.resolve(payloadDirectory);
  invariant(!fs.existsSync(finalDirectory),
    `payload output already exists: ${finalDirectory}`);

  const parentDirectory = path.dirname(finalDirectory);
  fs.mkdirSync(parentDirectory, { recursive: true });
  const stageDirectory = path.join(
    parentDirectory,
    `.${path.basename(finalDirectory)}.partial-${process.pid}-${Date.now()}-${crypto.randomBytes(6).toString("hex")}`,
  );

  try {
    const result = createPayloadInStaging(
      viewerPath,
      verificationPath,
      stageDirectory,
    );
    invariant(!fs.existsSync(finalDirectory),
      `payload output appeared during staging: ${finalDirectory}`);
    fs.renameSync(stageDirectory, finalDirectory);
    return {
      ...result,
      payloadDirectory: finalDirectory,
    };
  } catch (error) {
    fs.rmSync(stageDirectory, { recursive: true, force: true });
    throw error;
  }
}

if (require.main === module) {
  if (process.argv.length !== 5) {
    console.error(
      "usage: node review_html_to_skrv_payload.js <viewer.html> <verification.json> <payload-directory>",
    );
    process.exit(2);
  }
  try {
    const result = createPayload(...process.argv.slice(2));
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  } catch (error) {
    console.error(error && error.stack ? error.stack : String(error));
    process.exit(1);
  }
}

module.exports = {
  MESH_BINARY_KEYS,
  POSE_KEYS,
  createPayload,
  loadViewerData,
  sha256,
};
