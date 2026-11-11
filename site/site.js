// Mimic's page: the feature cards and the year
(function () {
  'use strict';
  const features = [
    ['Face swapper', '13 models: inswapper, HyperSwap 1a/1b/1c, SimSwap, Ghost, HiFiFace, BlendSwap, UniFace. Pixel boost for sharper big faces, and a weight between the source and the target.'],
    ['Face enhancer', 'GFPGAN 1.2/1.3/1.4, CodeFormer, GPEN 256 to 2048 and RestoreFormer++, blended back in.'],
    ['Face selector', 'Every face, the first, or only faces like one you click. Order them, and filter by age, gender and race.'],
    ['Face masker', 'Box, occlusion (hands, hair and glasses stay in front), area and region masks, with blur and padding.'],
    ['Detector and landmarker', 'RetinaFace, SCRFD, YOLO-Face or YuNet, turned for sideways faces; 2DFAN4 or PeppaWutz for 68 face points.'],
    ['Video', 'Through ffmpeg with the sound kept: x264, x265, VP9, NVENC, AMF or QSV, quality, fps and trim. On any GPU through DirectML.'],
    ['In LayerTack', 'Filter > Mimic > Face Swap puts the result in as a new layer, masked to the selection.']
  ];
  const grid = document.getElementById('features');
  if (grid) {
    for (const [title, text] of features) {
      const card = document.createElement('article');
      const h = document.createElement('h2');
      h.textContent = title;
      const p = document.createElement('p');
      p.textContent = text;
      card.append(h, p);
      grid.appendChild(card);
    }
  }
  const year = document.getElementById('year');
  if (year) year.textContent = String(Math.max(2026, new Date().getFullYear()));
})();
