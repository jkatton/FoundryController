const MODULE_ID = "hp-bridge";
const EXPOSED_FLAG = "exposed";

// Actor types eligible for the "Send to HP Monitor" toggle. Broad enough to
// cover the common combatant types across systems (dnd5e's character/npc/
// vehicle, pf2e's familiar, etc.) without picking up non-combatant types
// like loot or parties.
const EXPOSABLE_ACTOR_TYPES = ["character", "npc", "vehicle", "familiar"];

Hooks.on('renderActorSheet', async (app, html, data) => {
  // Normalize html to a real HTMLElement (Foundry v13+ passes DOM, older passes jQuery)
  const root = html instanceof HTMLElement ? html : html[0];
  if (!root) return console.warn('[hp-bridge] No root element on sheet render');


  const actor = app.actor ?? app.document;
  if (!actor || !EXPOSABLE_ACTOR_TYPES.includes(actor.type)) return;

  // Read-only sheets (no owner permission) still show the toggle, but
  // disabled, so users understand the option exists without being able
  // to change it.
  const editable = !!app.isEditable;

  // Avoid double-injection on re-renders
  if (root.querySelector('.hp-bridge-config-group')) return;

 const exposed = actor.getFlag(MODULE_ID, EXPOSED_FLAG) === true;

  const wrapper = document.createElement('div');
  wrapper.innerHTML = `
    <div class="form-group hp-bridge-config-group" style="border: 1px solid #7a7975; padding: 8px; margin: 8px 0; border-radius: 4px; background: rgba(0,0,0,0.1);${editable ? "" : " opacity: 0.6;"}">
      <label style="font-weight: bold; color: #4b4a44;"><i class="fas fa-joystick" title="When checked, this character's HP is included in the HP Bridge relay's data."></i> HP Bridge Link</label>
      <div class="form-fields">
        <input type="checkbox" ${exposed ? "checked" : ""} ${editable ? "" : "disabled"} name="flags.hp-bridge.exposed" style="margin:0; width:14px; height:14px; flex:0 0 auto;${editable ? "" : " cursor: not-allowed;"}" />
      </div>
      <p class="notes" style="margin: 4px 0 0 0; font-size: 11px; color: #4b4a44;">Send to HP Monitor${editable ? "" : " <em>(you need owner permission on this character to change this setting)</em>"}</p>
    </div>
  `;
  const node = wrapper.firstElementChild;

  // Try multiple known mount points in order of preference
  const target =
    root.querySelector('.tab[data-tab="details"]') ||
    root.querySelector('.tab[data-tab="description"]') ||
    root.querySelector('.tab.active') ||
    root.querySelector('.sheet-body') ||
    root.querySelector('form') ||
    root;

  target.append(node);
  console.log('[hp-bridge] Injected config into', target);
});