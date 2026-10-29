-- DragonDE configuration
--
-- Copy to ~/.config/dragon/config.lua and edit. It's reloaded on save (and
-- with Mod+Shift+R); if it has an error, the log says where and the previous
-- configuration stays in effect. Anything you don't set keeps its default,
-- so this file can be as short as you like. Every value below IS the default.

dragon.set {
    -- The "Mod" key in shortcuts below, and for Mod+drag (move) and
    -- Mod+right-drag (resize). "super", "alt", "ctrl", or "auto": Super,
    -- but Alt when DragonDE runs inside another desktop.
    modifier = "auto",

    terminal = "kitty",

    -- Colors are "#rgb", "#rrggbb" or "#rrggbbaa".
    background = "#141720",

    -- An image behind everything (PNG, JPEG, WebP, SVG, ...). "~/" works.
    -- mode: fill (cover, cropping) | fit (letterbox) | center | stretch | tile
    -- wallpaper = { path = "~/Pictures/wallpaper.png", mode = "fill" },
    wallpaper = { path = "", mode = "fill" },

    keyboard = {
        -- XKB names, like in /usr/share/X11/xkb/rules/evdev.lst. Leave empty
        -- for the system default (nested: whatever the parent desktop uses).
        layout = "",           -- e.g. "us", "de", "us,ru"
        variant = "",          -- e.g. "dvorak", "colemak"
        options = "",          -- e.g. "caps:escape,grp:alt_shift_toggle"
        model = "",
        repeat_rate = 25,      -- keys per second (0: no repeat)
        repeat_delay = 600,    -- ms before repeating starts
    },

    -- How windows look. This applies to every window: when an app draws its
    -- own title bar (GTK apps, Firefox), DragonDE still rounds its corners,
    -- adds the border and shadow and applies the opacity. Apps that let the
    -- compositor decide (kitty, foot, Qt apps, X11 apps) get our title bar.
    windows = {
        corner_radius = 10,

        -- A number for both, or { active = .., inactive = .., fullscreen = .. }. 0.05 to 1.
        opacity = { active = 1.0, inactive = 1.0, fullscreen = 1.0 },

        -- Darken windows without focus: a strength (0 = off, 1 = black),
        -- or { strength = .., color = .. }.
        dim = { strength = 0.0, color = "#000000" },

        border = {
            width = 2,
            -- A color, or a gradient { "#from", "#to" } along gradient_angle.
            active = "#5e82ab",
            inactive = "#3b4252",
            gradient_angle = 45,   -- degrees, 0 = left to right
        },

        shadow = {
            enabled = true,
            size = 20,             -- how far it fades out
            color = "#00000073",
            -- inactive_color = "#00000040", -- unfocused windows (default: color)
            power = 2,             -- falloff: higher is tighter and harder
            offset = { 0, 4 },     -- x, y
        },

        -- A colored halo around the window. Transparent "inactive" = none.
        glow = {
            enabled = false,
            size = 14,
            active = "#88c0d08c",
            inactive = "#00000000",
            power = 2.5,
        },

        -- Frosted glass behind see-through windows (opacity below 1, or apps
        -- with transparent backgrounds). Opaque windows skip it: it's free
        -- unless something is translucent. `blur = false` turns it off.
        blur = {
            enabled = true,
            size = 8,              -- spread per pass
            passes = 2,            -- 1-6, each doubles the reach (and the cost)
            noise = 0.0117,        -- fine grain against banding
            contrast = 0.8916,
            brightness = 0.8172,
            vibrancy = 0.1696,     -- saturation boost
            xray = false,          -- blur only the wallpaper (cheaper; windows behind don't show)
        },

        titlebar = {
            enabled = true,        -- false: apps draw their own (if they can)
            height = 30,
            font = "sans-serif",
            font_size = 10.5,
            bold = true,
            align = "center",                       -- left | center | right
            buttons = { "minimize", "maximize", "close" }, -- any order, any subset
            buttons_side = "right",                 -- left | right
            button_style = "icon",                  -- icon | circle
            button_size = 18,
            button_spacing = 6,
            padding = 8,
            colors = {
                active = {
                    background = "#292b36",
                    text = "#e6e8f0",
                    button = "#bfc4d1",
                    button_hover = "#4d5161",
                    close_hover = "#d94040",
                },
                inactive = {
                    background = "#1f2129",
                    text = "#8c919e",
                    button = "#80858f",
                    button_hover = "#3d424f",
                    close_hover = "#b33838",
                },
            },
        },
    },
}

-- Per-app overrides --------------------------------------------------------
--
-- dragon.rule(match, overrides): windows whose app_id and/or title match
-- (shell-style patterns, ignoring case: "*" anything, "?" one character) get
-- `overrides` on top of `windows` above. Any setting from `windows` works.
-- Later rules win. To find an app's app_id, look for "mapped" in the log
-- (DRAGON_LOG=debug): mapped 'title' (app_id).
--
-- dragon.rule({ app_id = "kitty" }, { opacity = { active = 0.95, inactive = 0.85 } })
-- dragon.rule({ app_id = "firefox" }, { corner_radius = 6 })
-- dragon.rule({ app_id = "mpv" }, { titlebar = { enabled = false }, border = { width = 0 }, shadow = { enabled = false } })
-- dragon.rule({ title = "*Picture-in-Picture*" }, { opacity = 0.9 })
-- dragon.rule({ app_id = "org.gnome.*" }, { border = { active = { "#88c0d0", "#b48ead" } } })

-- Layer surfaces (bars, launchers, notifications) ---------------------------
--
-- Drawn as the app made them unless a layer rule matches their namespace
-- (DRAGON_LOG=debug shows it: layer surface 'waybar' mapped ...).
--   blur = true | { size, passes, noise, contrast, brightness, vibrancy, xray }
--   ignore_alpha = 0.3   -- blur only where the surface is at least this opaque
--                        -- (a bar with gaps between its pieces)
--   opacity = 0.9
--   corner_radius = 12
--   shadow = true | { size, color, power }
--
-- dragon.layer_rule({ namespace = "waybar" }, { blur = true, ignore_alpha = 0.3 })
-- dragon.layer_rule({ namespace = "wofi" }, { blur = true, opacity = 0.9, corner_radius = 12, shadow = true })

-- Shortcuts --------------------------------------------------------------
--
-- dragon.bind(keys, action): `keys` is modifiers and a key joined with "+":
-- Mod, Super, Alt, Ctrl, Shift, then a key name (Return, Escape, Up, F11,
-- Print, XF86AudioRaiseVolume, a letter, ...). Keys match by their unshifted
-- symbol: Mod+Shift+2 is written that way, not Mod+@.
--
-- `action` is one of the built-ins:
--   "terminal"   start the terminal
--   "close"      ask the focused window to close
--   "quit"       leave DragonDE
--   "minimize"   hide the focused window
--   "restore"    bring back the most recently minimized window
--   "maximize"   toggle maximized
--   "fullscreen" toggle fullscreen
--   "reload"     reload this file
-- or a Lua function. Functions can use:
--   dragon.spawn(cmd)    run a shell command
--   dragon.terminal()    the configured terminal
--   dragon.log(...)      write to DragonDE's log
--   dragon.close(), dragon.quit(), dragon.minimize(), dragon.restore(),
--   dragon.maximize(), dragon.fullscreen(), dragon.reload()
--
-- The built-in shortcuts are bound even without these lines; they're listed
-- here so they're easy to change. Edit the keys, or dragon.unbind(keys) to
-- remove one.
dragon.bind("Mod+Return", "terminal")
dragon.bind("Mod+Q", "close")
dragon.bind("Mod+Shift+Escape", "quit")
dragon.bind("Mod+M", "minimize")
dragon.bind("Mod+Shift+M", "restore")
dragon.bind("Mod+Up", "maximize")
dragon.bind("Mod+F", "fullscreen")
dragon.bind("Mod+Shift+R", "reload")

-- App launcher.
dragon.bind("Mod+D", function() dragon.spawn("wofi --show drun") end)

-- More examples:
-- dragon.bind("Mod+B", function() dragon.spawn("firefox") end)
-- dragon.bind("Mod+E", function() dragon.spawn("dolphin") end)
-- dragon.bind("Mod+Shift+Return", function() dragon.spawn(dragon.terminal() .. " --hold htop") end)
-- dragon.bind("Print", function() dragon.spawn("grim ~/screenshot.png") end)
-- dragon.unbind("Mod+Up")
