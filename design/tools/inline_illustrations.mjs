// Вставляет design/export/illustrations/*.svg в screens/06-icon.html между <!--ill:имя--> и <!--/ill-->.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const page = join(root, "screens", "06-icon.html");
const html = readFileSync(page, "utf8").replace(/<!--ill:([a-z-]+)-->[\s\S]*?<!--\/ill-->/g, (_, name) => {
  const svg = readFileSync(join(root, "export", "illustrations", name + ".svg"), "utf8")
    .replace(/<style>[\s\S]*?<\/style>\s*/, "").replace(/\s*\n\s*/g, "").trim();
  return `<!--ill:${name}-->${svg}<!--/ill-->`;
});
writeFileSync(page, html);
