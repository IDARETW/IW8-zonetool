// mw120r IW3 compatibility layer for Replay 1.20 (IW8 GSC, compiled with
// gsc-tool -g iw8). The IW3 map script translator appends this to every
// converted map script and routes IW3 common-script calls here:
//   maps\mp\_load::main()        -> iw3compat_load_main()
//   maps\mp\_compass::*          -> handled by iw3compat_minimap() (generated)
//   maps\mp\_utility::exploder() -> iw3compat_exploder()
//   playsound / playloopsound / playsoundatpos / soundexists -> iw3compat_*,
//   which play only the aliases converted into the map bank (iw3compat_sounds())
// Builtin ids, argument orders and field tokens were checked against the
// Replay 1.20 builtin tables and handlers. Effects come from common MP zones
// (loadfx only accepts fx/ or vfx/ names). The converter renames IW3-only
// targetnames (destructible, interactive_tv, exploder) to mw120r_* so stock
// Replay scripts never adopt these entities.

iw3compat_noop()
{
}

// Models the converter included and precached (iw3compat_generated()).
iw3compat_known_model( name )
{
    if ( isdefined( level.mw120r_models ) && isdefined( level.mw120r_models[name] ) )
        return name;
    return undefined;
}

// The destroyed model of an IW3 destructible type (generated per map).
iw3compat_destroyed_model( type )
{
    if ( isdefined( level.mw120r_destroyed ) && isdefined( level.mw120r_destroyed[type] ) )
        return iw3compat_known_model( level.mw120r_destroyed[type] );
    return iw3compat_known_model( type + "_destroyed" );
}

iw3compat_load_main()
{
    if ( isdefined( level.mw120r_iw3_loaded ) )
        return;

    level.mw120r_iw3_loaded = 1;
    iw3compat_precache();
    iw3compat_remove_gameobjects();
    iw3compat_exploders_init();
    iw3compat_global_fx();
    level thread iw3compat_destructible_vehicles();
    level thread iw3compat_destructables();
    level thread iw3compat_interactive_tvs();
    level thread iw3compat_out_of_bounds();
}

// IW8 maps kill players who leave the play space; converted IW3 maps had no
// such volume, and far outside the map Replay's visibility pass crashed.
iw3compat_out_of_bounds()
{
    if ( !isdefined( level.mw120r_playspace_min ) || !isdefined( level.mw120r_playspace_max ) )
        return;

    low = level.mw120r_playspace_min;
    high = level.mw120r_playspace_max;
    for ( ;; )
    {
        wait 0.25;
        if ( !isdefined( level.players ) )
            continue;

        foreach ( player in level.players )
        {
            // Only builtins Replay registers (isalive/suicide compile with
            // gsc-tool but are unknown to Replay's linker: "unknown function").
            if ( !isdefined( player ) || !isdefined( player.health ) || player.health <= 0 )
                continue;

            origin = player.origin;
            if ( origin[0] < low[0] || origin[1] < low[1] || origin[2] < low[2] || origin[0] > high[0] || origin[1] > high[1] || origin[2] > high[2] )
                radiusdamage( origin, 32, 100000, 100000 );
        }
    }
}

iw3compat_precache()
{
    // Native glass gives 15% of unsupported shards a physics body instead of
    // the scripted fall ("LLTLMSKMQ", the shard physics chance). On converted
    // maps those shards stayed where the pane broke, floating in the frame;
    // with the chance at 0 every unsupported shard takes the scripted fall.
    setdvar( "LLTLMSKMQ", "0" );
    iw3compat_sounds();
    iw3compat_model_parts();
    // Converted IW3 effects (generated per map). A slot loads its stock
    // stand-in only when the IW3 effect did not convert, so each slot holds
    // exactly one loaded effect.
    iw3compat_iw3fx();
    level.mw120r_fx = [];
    level.mw120r_fx["veh_smoke_light"] = iw3compat_fx( "smoke/car_damage_whitesmoke", "vfx/iw8/veh/scriptables/vfx_veh_hood_smoke" );
    level.mw120r_fx["veh_smoke_heavy"] = iw3compat_fx( "smoke/car_damage_blacksmoke", "vfx/iw8/veh/scriptables/vfx_smoke_heavy_ch" );
    level.mw120r_fx["veh_fire"] = iw3compat_fx( "smoke/car_damage_blacksmoke_fire", "vfx/iw8/veh/scriptables/vfx_veh_fire_engine_flareup" );
    level.mw120r_fx["veh_explosion"] = iw3compat_fx( "explosions/small_vehicle_explosion", "vfx/iw8/veh/scriptables/vfx_veh_explosion_civ" );
    level.mw120r_fx["glass_shatter"] = iw3compat_fx( "impacts/small_glass", "vfx/code/glass/glass_shatter_32x32" );
    level.mw120r_fx["sparks"] = iw3compat_fx( "impacts/small_metalhit_1", "vfx/iw8_mp/equipment/flashbang/vfx_spark_crack_npop" );
    // IW3's own explosion carries its fire; only the stock stand-in needs one.
    if ( iw3compat_slot_stock( "veh_explosion" ) )
        level.mw120r_fx["veh_fire_linger"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_fire_linger_med" );
}

iw3compat_has_iw3fx( iw3name )
{
    return isdefined( level.mw120r_iw3fx ) && isdefined( level.mw120r_iw3fx[iw3name] );
}

// A slot holds a stock Replay effect when the IW3 effect did not convert, or
// when --style iw8 maps it to a stock one (level.mw120r_iw3fx_stock, generated).
iw3compat_fx_is_stock( iw3name )
{
    if ( !iw3compat_has_iw3fx( iw3name ) )
        return 1;
    return isdefined( level.mw120r_iw3fx_stock ) && isdefined( level.mw120r_iw3fx_stock[iw3name] );
}

iw3compat_slot_stock( key )
{
    if ( key == "veh_smoke_light" )
        return iw3compat_fx_is_stock( "smoke/car_damage_whitesmoke" );
    if ( key == "veh_smoke_heavy" )
        return iw3compat_fx_is_stock( "smoke/car_damage_blacksmoke" );
    if ( key == "veh_fire" )
        return iw3compat_fx_is_stock( "smoke/car_damage_blacksmoke_fire" );
    if ( key == "veh_explosion" )
        return iw3compat_fx_is_stock( "explosions/small_vehicle_explosion" );
    return 1;
}

// IW3 loadfx( name ) with a runtime name: the converted effect when the map
// scripts name it as a literal (loaded by iw3compat_iw3fx()), else a stock
// stand-in so callers always get a valid effect.
iw3compat_loadfx( iw3name )
{
    if ( isdefined( iw3name ) && iw3compat_has_iw3fx( tolower( iw3name ) ) )
        return level.mw120r_iw3fx[tolower( iw3name )];
    return loadfx( "vfx/iw8_mp/equipment/flashbang/vfx_spark_crack_npop" );
}

iw3compat_fx( iw3name, stock )
{
    if ( iw3compat_has_iw3fx( iw3name ) )
        return level.mw120r_iw3fx[iw3name];
    return loadfx( stock );
}

// ---------------------------------------------------------------------------
// Sounds: level.mw120r_sounds[alias] is 1 for a one-shot, 2 for a loop.
// ---------------------------------------------------------------------------
iw3compat_sound_kind( alias )
{
    if ( !isdefined( level.mw120r_sounds ) )
        iw3compat_sounds();
    if ( !isdefined( alias ) || !isdefined( level.mw120r_sounds[alias] ) )
        return 0;
    return level.mw120r_sounds[alias];
}

iw3compat_playsound( alias, unused1, unused2 )
{
    if ( iw3compat_sound_kind( alias ) == 1 )
        self playsound( alias );
}

iw3compat_playloopsound( alias, unused1 )
{
    if ( iw3compat_sound_kind( alias ) == 2 )
        self playloopsound( alias );
}

iw3compat_stoploopsound( unused1 )
{
    self stoploopsound();
}

iw3compat_playsoundatpos( origin, alias )
{
    if ( isdefined( origin ) && iw3compat_sound_kind( alias ) == 1 )
        playsoundatpos( origin, alias );
}

iw3compat_playsoundinspace( alias, origin, unused1 )
{
    if ( !isdefined( origin ) )
        origin = self.origin;
    iw3compat_playsoundatpos( origin, alias );
}

iw3compat_soundexists( alias )
{
    return iw3compat_sound_kind( alias ) != 0;
}

// IW3 ambientPlay is a 2D loop for everyone. Each player carries the converted
// loop, so it plays at the listener at full volume (players who connect later
// get it too).
iw3compat_ambientplay( alias, unused1 )
{
    if ( iw3compat_sound_kind( alias ) != 2 )
        return;
    level notify( "mw120r_ambient" );
    level.mw120r_ambient = alias;
    if ( isdefined( level.players ) )
    {
        foreach ( player in level.players )
            player iw3compat_ambient_start();
    }
    level thread iw3compat_ambient_connect();
}

iw3compat_ambientstop( unused1 )
{
    level notify( "mw120r_ambient" );
    level.mw120r_ambient = undefined;
    if ( isdefined( level.players ) )
    {
        foreach ( player in level.players )
            player stoploopsound();
    }
}

iw3compat_ambient_connect()
{
    level endon( "mw120r_ambient" );
    for ( ;; )
    {
        level waittill( "connected", player );
        player iw3compat_ambient_start();
    }
}

iw3compat_ambient_start()
{
    if ( isdefined( level.mw120r_ambient ) )
        self playloopsound( level.mw120r_ambient );
}

// IW3 deleted every game object its gametype did not use. Replay's own
// gametypes never use the IW3 ones, so drop their props in TDM/FFA.
iw3compat_remove_gameobjects()
{
    gametype = tolower( getdvar( "g_gametype" ) );
    if ( gametype != "war" && gametype != "dm" && gametype != "tdm" )
        return;

    models = getentarray( "script_model", "classname" );
    foreach ( model in models )
    {
        if ( isdefined( model.script_gameobjectname ) )
            model delete();
    }
}

// ---------------------------------------------------------------------------
// Exploders: IW3 hides the "exploder" (damaged) state until exploder( n ).
// ---------------------------------------------------------------------------
iw3compat_exploders_init()
{
    level.mw120r_exploders = getentarray( "mw120r_exploder", "targetname" );
    foreach ( piece in level.mw120r_exploders )
        piece hide();
}

iw3compat_exploder( number )
{
    if ( !isdefined( level.mw120r_exploders ) )
        return;

    number = "" + number;
    foreach ( piece in level.mw120r_exploders )
    {
        if ( isdefined( piece.script_exploder ) && ( "" + piece.script_exploder ) == number )
            piece show();
    }

    models = getentarray( "script_model", "classname" );
    foreach ( model in models )
    {
        if ( !isdefined( model.script_exploder ) || ( "" + model.script_exploder ) != number )
            continue;
        if ( isdefined( model.targetname ) && model.targetname == "mw120r_exploder" )
            continue;
        model delete();
    }
}

// ---------------------------------------------------------------------------
// createfx placement (maps\createfx\<map>_fx): IW3 _utility createEffect()
// records, played as IW3 _fx did once the map's main has run. One-shots are
// fx entities (looping effects keep playing), loop effects repeat every
// v["delay"] seconds, loop sounds play converted looping aliases.
// ---------------------------------------------------------------------------
iw3compat_createfx_add( type, fxid, delay )
{
    if ( !isdefined( level.mw120r_createfx ) )
    {
        level.mw120r_createfx = [];
        level thread iw3compat_createfx_start();
    }
    ent = spawnstruct();
    ent.v = [];
    ent.v["type"] = type;
    if ( isdefined( fxid ) )
        ent.v["fxid"] = fxid;
    ent.v["angles"] = ( 0, 0, 0 );
    ent.v["origin"] = ( 0, 0, 0 );
    ent.v["delay"] = delay;
    level.mw120r_createfx[level.mw120r_createfx.size] = ent;
    return ent;
}

iw3compat_createoneshoteffect( fxid )
{
    return iw3compat_createfx_add( "oneshotfx", fxid, -15 );
}

iw3compat_createloopeffect( fxid )
{
    return iw3compat_createfx_add( "loopfx", fxid, 0.5 );
}

iw3compat_createloopsound()
{
    return iw3compat_createfx_add( "soundfx", undefined, 0 );
}

// Exploder effects only play with their exploder, which the converted
// exploder() does not drive; they are recorded and not played.
iw3compat_createexploder( fxid )
{
    return iw3compat_createfx_add( "exploder", fxid, 0 );
}

// One IW3 _global_fx placement (generated by the converter from the map's
// script_structs): a createfx one-shot with IW3's random -20..-15 s offset.
iw3compat_global_fx_one( iw3name, origin, angles, alias )
{
    if ( !iw3compat_has_iw3fx( iw3name ) )
        return;
    if ( !isdefined( level._effect ) )
        level._effect = [];
    level._effect[iw3name] = level.mw120r_iw3fx[iw3name];
    ent = iw3compat_createfx_add( "oneshotfx", iw3name, randomfloatrange( -20, -15 ) );
    ent.v["origin"] = origin;
    ent.v["angles"] = angles;
    if ( alias != "" )
        ent.v["soundalias"] = alias;
}

// IW3 maps\mp\_fx::loopfx( fxId, fxPos, waittime, fxPos2 ): a createfx loop
// effect facing fxPos2.
iw3compat_loopfx( fxid, origin, delay, origin2, unused1, unused2, unused3 )
{
    ent = iw3compat_createloopeffect( fxid );
    ent.v["origin"] = origin;
    if ( isdefined( origin2 ) )
        ent.v["angles"] = vectortoangles( origin2 - origin );
    ent.v["delay"] = delay;
}

// IW3 maps\mp\_fx::loopSound / soundfx: a looping alias at a point.
iw3compat_loopsound( alias, origin, unused1 )
{
    iw3compat_soundfx( alias, origin );
}

iw3compat_soundfx( alias, origin, endonnotify )
{
    if ( iw3compat_sound_kind( alias ) != 2 )
        return;
    org = spawn( "script_origin", origin );
    org playloopsound( alias );
    if ( isdefined( endonnotify ) )
        org thread iw3compat_soundfx_delete( endonnotify );
}

iw3compat_soundfx_delete( endonnotify )
{
    level waittill( endonnotify );
    self delete();
}

// IW3 maps\mp\gametypes\_shellshock::barrel_earthQuake (self = the barrel).
iw3compat_barrel_earthquake()
{
    earthquake( 0.4, 0.5, self.origin, 512 );
}

// ---------------------------------------------------------------------------
// IW3 common_scripts\utility / maps\mp\_utility helpers.
// ---------------------------------------------------------------------------
iw3compat_array_thread( entities, process, var1, var2, var3 )
{
    keys = getarraykeys( entities );
    for ( i = 0; i < keys.size; i++ )
    {
        if ( isdefined( var3 ) )
            entities[keys[i]] thread [[ process ]]( var1, var2, var3 );
        else if ( isdefined( var2 ) )
            entities[keys[i]] thread [[ process ]]( var1, var2 );
        else if ( isdefined( var1 ) )
            entities[keys[i]] thread [[ process ]]( var1 );
        else
            entities[keys[i]] thread [[ process ]]();
    }
}

iw3compat_array_levelthread( ents, process, var, excluders )
{
    for ( i = 0; i < ents.size; i++ )
    {
        excluded = 0;
        if ( isdefined( excluders ) )
        {
            for ( p = 0; p < excluders.size; p++ )
            {
                if ( ents[i] == excluders[p] )
                    excluded = 1;
            }
        }
        if ( excluded )
            continue;
        if ( isdefined( var ) )
            level thread [[ process ]]( ents[i], var );
        else
            level thread [[ process ]]( ents[i] );
    }
}

// A missing tag plays at the entity instead of raising a script error.
iw3compat_play_sound_on_tag( alias, tag )
{
    if ( iw3compat_sound_kind( alias ) != 1 )
        return;
    at = undefined;
    if ( isdefined( tag ) )
        at = self gettagorigin( tag, 1 );
    if ( isdefined( at ) )
    {
        org = spawn( "script_origin", at );
        org linkto( self, tag, ( 0, 0, 0 ), ( 0, 0, 0 ) );
    }
    else
    {
        org = spawn( "script_origin", self.origin );
        org.angles = self.angles;
        org linkto( self );
    }
    org playsound( alias );
    wait 5;
    org delete();
}

// IW3 getstructarray( name, type ) over the map's keyed script_structs
// (generated: iw3compat_structs()), indexed like IW3 struct_class_init.
iw3compat_getstructarray( name, type )
{
    if ( !isdefined( level.mw120r_struct_names ) )
    {
        level.mw120r_struct_names = [];
        level.mw120r_struct_names["targetname"] = [];
        level.mw120r_struct_names["target"] = [];
        level.mw120r_struct_names["script_noteworthy"] = [];
        level.mw120r_struct_names["script_linkname"] = [];
        iw3compat_structs();
    }
    if ( !isdefined( name ) || !isdefined( type ) || !isdefined( level.mw120r_struct_names[type] ) )
        return [];
    if ( !isdefined( level.mw120r_struct_names[type][name] ) )
        return [];
    return level.mw120r_struct_names[type][name];
}

iw3compat_struct_add( origin, angles, targetname, target, noteworthy, linkname )
{
    s = spawnstruct();
    s.origin = origin;
    s.angles = angles;
    if ( targetname != "" )
    {
        s.targetname = targetname;
        iw3compat_struct_index( "targetname", targetname, s );
    }
    if ( target != "" )
    {
        s.target = target;
        iw3compat_struct_index( "target", target, s );
    }
    if ( noteworthy != "" )
    {
        s.script_noteworthy = noteworthy;
        iw3compat_struct_index( "script_noteworthy", noteworthy, s );
    }
    if ( linkname != "" )
    {
        s.script_linkname = linkname;
        iw3compat_struct_index( "script_linkname", linkname, s );
    }
}

iw3compat_struct_index( type, name, s )
{
    if ( !isdefined( level.mw120r_struct_names[type][name] ) )
        level.mw120r_struct_names[type][name] = [];
    size = level.mw120r_struct_names[type][name].size;
    level.mw120r_struct_names[type][name][size] = s;
}

iw3compat_createfx_start()
{
    wait 0.05;
    foreach ( ent in level.mw120r_createfx )
        ent thread iw3compat_createfx_play();
}

iw3compat_createfx_play()
{
    type = self.v["type"];
    origin = self.v["origin"];
    forward = anglestoforward( self.v["angles"] );
    up = anglestoup( self.v["angles"] );
    if ( type == "soundfx" )
    {
        alias = self.v["soundalias"];
        if ( iw3compat_sound_kind( alias ) != 2 )
            return;
        self.looper = spawn( "script_origin", origin );
        self.looper playloopsound( alias );
        return;
    }
    if ( !isdefined( self.v["fxid"] ) || !isdefined( level._effect ) || !isdefined( level._effect[self.v["fxid"]] ) )
        return;
    fx = level._effect[self.v["fxid"]];
    delay = self.v["delay"];
    // IW3 createfx one-shots and loop effects may carry a looping sound at
    // their origin (_fx create_loopsound).
    if ( ( type == "oneshotfx" || type == "loopfx" ) && isdefined( self.v["soundalias"] ) &&
         iw3compat_sound_kind( self.v["soundalias"] ) == 2 )
    {
        self.sound = spawn( "script_origin", origin );
        self.sound playloopsound( self.v["soundalias"] );
    }
    if ( type == "oneshotfx" )
    {
        if ( delay > 0 )
            wait delay;
        self.looper = spawnfx( fx, origin, forward, up );
        if ( delay > 0 )
            delay = 0;
        triggerfx( self.looper, delay );
    }
    else if ( type == "loopfx" )
    {
        if ( delay < 0.05 )
            delay = 0.05;
        for ( ;; )
        {
            playfx( fx, origin, forward, up );
            wait delay;
        }
    }
}

// ---------------------------------------------------------------------------
// Destructible vehicles (IW3 maps\mp\_destructible_types vehicle_80s_* stages):
// 250 damage -> hood smoke, +200 -> heavy smoke, +100 -> fire that drains
// 12 per 0.2s through the next 300 + 400, then the explosion (375 radius,
// 300..20 damage). IW3 plays a state's effects when the state runs out.
// Melee never damages a vehicle ("no_melee").
// IW3 _destructible scales splash damage by 2.75 and, because the vehicle's
// first five states share its model, applies each splash hit to all five,
// less 0.4 * the distance from the explosion. A grenade beside a car sets it
// straight to its explosion, as in IW3.
// The converted model keeps the IW3 skeleton, so parts (glass, lights,
// mirrors) break by bone as in IW3: see iw3compat_vehicle_parts_init().
// ---------------------------------------------------------------------------
iw3compat_is_splash( type )
{
    if ( !isdefined( type ) )
        return 0;
    type = tolower( type );
    return type == "mod_grenade" || type == "mod_grenade_splash" || type == "mod_projectile" ||
           type == "mod_projectile_splash" || type == "mod_explosive" || type == "splash";
}

// IW3 getDamageType(): melee, splash, impact, or bullet (everything else).
iw3compat_damage_kind( type )
{
    if ( !isdefined( type ) )
        return "bullet";
    type = tolower( type );
    if ( type == "mod_melee" || type == "mod_crush" || type == "melee" )
        return "melee";
    if ( iw3compat_is_splash( type ) )
        return "splash";
    if ( type == "mod_impact" )
        return "impact";
    return "bullet";
}

iw3compat_vehicle_splash( amount, point )
{
    if ( !isdefined( point ) )
        point = self.origin;
    hit = amount * 2.75 - distance( point, self.origin ) * 0.4;
    if ( hit <= 0 )
        return 0;
    return hit * 5;
}

// Replay kills a damageable script entity whose health runs out (ending these
// threads on "death"); IW3 destructibles count their own damage instead.
iw3compat_make_damageable()
{
    self.maxhealth = 1000000;
    self.health = 1000000;
    self setcandamage( 1 );
    self setcanradiusdamage( 1 );
}

iw3compat_destructible_vehicles()
{
    vehicles = getentarray( "mw120r_destructible", "targetname" );
    foreach ( vehicle in vehicles )
    {
        if ( isdefined( vehicle.destructible_type ) && issubstr( tolower( vehicle.destructible_type ), "vehicle" ) )
            vehicle thread iw3compat_vehicle_think();
    }
}

iw3compat_vehicle_think()
{
    self endon( "death" );
    self iw3compat_make_damageable();
    self iw3compat_vehicle_parts_init();
    self.mw120r_destroyed_model = iw3compat_destroyed_model( self.destructible_type );
    self.mw120r_stage = 0;
    self.mw120r_damage = 0;
    self.mw120r_attacker = undefined;
    thresholds = [];
    thresholds[0] = 250;
    thresholds[1] = 450;
    thresholds[2] = 550;
    thresholds[3] = 1250;

    for (;;)
    {
        self waittill( "damage", amount, attacker, direction, point, type );
        self.health = 1000000;
        kind = iw3compat_damage_kind( type );
        if ( isdefined( attacker ) )
            self.mw120r_attacker = attacker;
        if ( kind == "splash" )
        {
            self iw3compat_vehicle_parts_splash( amount, point );
            amount = self iw3compat_vehicle_splash( amount, point );
        }
        else
        {
            // A hit goes to the part it struck; the body takes what IW3 passes
            // on (alsoDamageParent), or the whole hit when no part was struck.
            tag = self iw3compat_vehicle_hit_part( point );
            if ( isdefined( tag ) )
                amount = self iw3compat_vehicle_damage_part( tag, amount, kind );
            // The body itself ignores melee and impacts ("no_melee").
            if ( kind == "melee" || kind == "impact" )
                amount = 0;
        }

        self.mw120r_damage = self.mw120r_damage + amount;

        while ( self.mw120r_stage < 4 && self.mw120r_damage >= thresholds[self.mw120r_stage] )
        {
            self.mw120r_stage++;
            if ( self.mw120r_stage == 1 )
                self thread iw3compat_vehicle_loopfx( "veh_smoke_light", 1, 0.4 );
            else if ( self.mw120r_stage == 2 )
                self thread iw3compat_vehicle_loopfx( "veh_smoke_heavy", 2, 0.4 );
            else if ( self.mw120r_stage == 3 )
            {
                // IW3 fire state: flare-up plus the burning loop until the explosion.
                self iw3compat_playsound( "fire_vehicle_flareup_med" );
                self iw3compat_playloopsound( "fire_vehicle_med" );
                self thread iw3compat_vehicle_loopfx( "veh_fire", 3, 0.4 );
                self thread iw3compat_vehicle_drain();
            }
        }

        if ( self.mw120r_stage >= 4 )
            break;
    }

    self iw3compat_vehicle_explode();
}

iw3compat_vehicle_drain()
{
    self endon( "death" );
    self endon( "mw120r_exploded" );
    while ( self.mw120r_stage == 3 )
    {
        wait 0.2;
        self.mw120r_damage = self.mw120r_damage + 12;
        if ( self.mw120r_damage >= 1250 )
        {
            // The damage loop is blocked in waittill; wake it with a zero hit.
            self notify( "damage", 0, self.mw120r_attacker, ( 0, 0, 0 ), self.origin, "MOD_EXPLOSIVE" );
            return;
        }
    }
}

// Stock Replay vehicle effects are authored in the vehicle's frame (X forward,
// Z up) above a ground-level origin; IW3 effects keep IW3's tags and axes.
iw3compat_vehicle_playfx( fx, origin )
{
    playfx( fx, origin, anglestoforward( self.angles ), anglestoup( self.angles ) );
}

// Stock vehicle effects add their own 20-30 units of height above the point
// they play at (vfx_veh_fire_linger boxes at z 20..30), so they start at the
// engine bay rather than on IW3's hood tag.
iw3compat_vehicle_hood_stock()
{
    return self.origin + anglestoforward( self.angles ) * 45 + anglestoup( self.angles ) * 16;
}

iw3compat_vehicle_hood()
{
    return self.origin + anglestoforward( self.angles ) * 64 + ( 0, 0, 40 );
}

// IW3 destructible_loopfx( "tag_hood_fx", fx, rate ): playfxontag each period.
iw3compat_vehicle_loopfx( name, stage, rate )
{
    self endon( "death" );
    self endon( "mw120r_exploded" );
    ontag = isdefined( self gettagorigin( "tag_hood_fx", 1 ) );
    // The stock hood fire (vfx_veh_fire_linger) burns 20 s per play; replayed
    // on IW3's 0.4 s rhythm it stacked into a wall of fire drifting off the car
    // (owner tests 2026-10-14). It plays once, as an effect entity removed when
    // the stage ends or the vehicle explodes.
    if ( name == "veh_fire" && iw3compat_slot_stock( name ) )
    {
        fx = spawnfx( level.mw120r_fx[name], self iw3compat_vehicle_hood_stock(), anglestoforward( self.angles ), anglestoup( self.angles ) );
        triggerfx( fx );
        self.mw120r_stagefx = fx;
        while ( self.mw120r_stage == stage )
            wait 0.2;
        self.mw120r_stagefx = undefined;
        fx delete();
        return;
    }
    while ( self.mw120r_stage == stage )
    {
        // Stock Replay effects rise along their Z axis; IW3's tag_hood_fx
        // points X up, so they burned sideways on the tag.
        if ( iw3compat_slot_stock( name ) )
        {
            // Stock smoke particles live 2.5-5 s; one burst a second keeps the
            // plume without IW3's 0.4 s rhythm (frame-rate cost, owner 2026-10-14).
            self iw3compat_vehicle_playfx( level.mw120r_fx[name], self iw3compat_vehicle_hood_stock() );
            wait max( rate, 1 );
            continue;
        }
        if ( ontag )
            playfxontag( level.mw120r_fx[name], self, "tag_hood_fx" );
        else
            playfx( level.mw120r_fx[name], self iw3compat_vehicle_hood() );
        wait rate;
    }
}

iw3compat_vehicle_explode()
{
    self notify( "mw120r_exploded" );
    if ( isdefined( self.mw120r_stagefx ) )
    {
        self.mw120r_stagefx delete();
        self.mw120r_stagefx = undefined;
    }
    self setcandamage( 0 );
    self stoploopsound();
    self iw3compat_playsound( "car_explode" );
    // IW3: the explosion effect at tag_death_fx facing up, and the radius
    // damage 80 units above the origin.
    fxorigin = self gettagorigin( "tag_death_fx", 1 );
    if ( !isdefined( fxorigin ) )
        fxorigin = self.origin + ( 0, 0, 32 );
    // Stock vehicle effects place their own heights above a ground-level vehicle
    // origin (linger fire boxes at z -2..40); from IW3's raised tags they hovered
    // over the wreck (owner test 2026-10-14).
    if ( iw3compat_slot_stock( "veh_explosion" ) )
        self iw3compat_vehicle_playfx( level.mw120r_fx["veh_explosion"], self.origin );
    else
        playfx( level.mw120r_fx["veh_explosion"], fxorigin, ( 0, 0, 1 ), ( 1, 0, 0 ) );
    center = self.origin + ( 0, 0, 80 );

    attacker = self.mw120r_attacker;
    if ( isdefined( attacker ) )
        radiusdamage( center, 375, 300, 20, attacker, "MOD_EXPLOSIVE" );
    else
        radiusdamage( center, 375, 300, 20 );

    physicsexplosionsphere( center, 375, 0, 4 );
    earthquake( 0.4, 0.8, center, 1000 );

    if ( isdefined( self.mw120r_destroyed_model ) )
        self setmodel( self.mw120r_destroyed_model );

    // IW3 _destructible: the explode state plays the type's destroy anim
    // (setanimknob %vehicle_80s_sedan1_destroy, converted XAnimParts) and
    // explodeAnim()'s hop on the destroyed model.
    if ( isdefined( level.mw120r_destroyanim ) && isdefined( self.destructible_type ) &&
         isdefined( level.mw120r_destroyanim[self.destructible_type] ) )
        self scriptmodelplayanim( level.mw120r_destroyanim[self.destructible_type] );
    self thread iw3compat_vehicle_explode_hop();

    // IW3's own explosion carries its fire; the stock stand-in needs a burn.
    if ( iw3compat_slot_stock( "veh_explosion" ) )
        self thread iw3compat_vehicle_burn( 15 );
}

// IW3 _destructible explodeAnim(): the wreck jumps 16 units and pitches 10
// degrees over 0.3 s, then settles back.
iw3compat_vehicle_explode_hop()
{
    self endon( "death" );
    self movez( 16, 0.3, 0, 0.2 );
    self rotatepitch( 10, 0.3, 0, 0.2 );
    wait 0.3;
    self movez( -16, 0.3, 0.15, 0 );
    self rotatepitch( -10, 0.3, 0.15, 0 );
}

// The stock wreck fire (vfx_veh_fire_linger_med) burns 15-130 s per play on a
// ground-level vehicle origin: one play, removed after the burn. Replaying it
// every 0.5 s stacked 24 of them into the floating wall (owner tests).
iw3compat_vehicle_burn( seconds )
{
    // Starting with the blast stacked the fire's opening bursts on the stock
    // explosion's (frame drops as explosions start, owner test 2026-10-14).
    wait 1;
    if ( !isdefined( self ) )
        return;
    fx = spawnfx( level.mw120r_fx["veh_fire_linger"], self.origin, anglestoforward( self.angles ), anglestoup( self.angles ) );
    triggerfx( fx );
    wait seconds;
    fx delete();
}

// ---------------------------------------------------------------------------
// Vehicle parts (IW3 destructible_part, vehicle_80s_* values). A part's damaged
// state (tag_*_d) starts hidden. Glass cracks at its first health and shatters
// at its second (effect and sound at tag_*_fx); lights break once and show
// their damaged state; mirrors are knocked off. Doors, hood, trunk and the
// front bumper pass hits on to the body; wheels and the rear bumper absorb
// them. Parts only exist when the model has their bones.
// ---------------------------------------------------------------------------
iw3compat_vehicle_parts_init()
{
    self.mw120r_parts = [];
    self.mw120r_part_min = undefined;
    self.mw120r_part_max = undefined;
    if ( isdefined( self.model ) && isdefined( level.mw120r_part_min ) && isdefined( level.mw120r_part_min[self.model] ) )
    {
        self.mw120r_part_min = level.mw120r_part_min[self.model];
        self.mw120r_part_max = level.mw120r_part_max[self.model];
    }
    self iw3compat_vehicle_part( "tag_glass_front", 40, "tag_glass_front_d", 60, "tag_glass_front_fx", "props/car_glass_large", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_glass_back", 40, "tag_glass_back_d", 60, "tag_glass_back_fx", "props/car_glass_large", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_glass_left_front", 20, "tag_glass_left_front_d", 60, "tag_glass_left_front_fx", "props/car_glass_med", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_glass_right_front", 20, "tag_glass_right_front_d", 60, "tag_glass_right_front_fx", "props/car_glass_med", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_glass_left_back", 20, "tag_glass_left_back_d", 60, "tag_glass_left_back_fx", "props/car_glass_med", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_glass_right_back", 20, "tag_glass_right_back_d", 60, "tag_glass_right_back_fx", "props/car_glass_med", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_light_left_front", 20, "tag_light_left_front_d", undefined, "tag_light_left_front", "props/car_glass_headlight", "window_entry_glass_med", 0.5 );
    self iw3compat_vehicle_part( "tag_light_right_front", 20, "tag_light_right_front_d", undefined, "tag_light_right_front", "props/car_glass_headlight", "window_entry_glass_med", 0.5 );
    self iw3compat_vehicle_part( "tag_light_left_back", 20, "tag_light_left_back_d", undefined, "tag_light_left_back", "props/car_glass_brakelight", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_light_right_back", 20, "tag_light_right_back_d", undefined, "tag_light_right_back", "props/car_glass_brakelight", "window_entry_glass_med", undefined );
    self iw3compat_vehicle_part( "tag_mirror_left", 40, undefined, undefined, undefined, undefined, undefined, undefined );
    self iw3compat_vehicle_part( "tag_mirror_right", 40, undefined, undefined, undefined, undefined, undefined, undefined );
    self iw3compat_vehicle_part( "tag_hood", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_trunk", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_door_left_front", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_door_left_back", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_door_right_front", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_door_right_back", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_bumper_front", undefined, undefined, undefined, undefined, undefined, undefined, 1 );
    self iw3compat_vehicle_part( "tag_bumper_back", undefined, undefined, undefined, undefined, undefined, undefined, undefined );
    // Wheel meshes ride the wheel joints, which IW3 never damages.
    self iw3compat_vehicle_part( "left_wheel_01_jnt", undefined, undefined, undefined, undefined, undefined, undefined, undefined );
    self iw3compat_vehicle_part( "left_wheel_02_jnt", undefined, undefined, undefined, undefined, undefined, undefined, undefined );
    self iw3compat_vehicle_part( "right_wheel_01_jnt", undefined, undefined, undefined, undefined, undefined, undefined, undefined );
    self iw3compat_vehicle_part( "right_wheel_02_jnt", undefined, undefined, undefined, undefined, undefined, undefined, undefined );
}

// gettagorigin( tag, 1 ) returns undefined instead of raising a script error
// when the model lacks the tag (GScr_UpdateTagInternal).
iw3compat_vehicle_part( tag, health, dtag, dhealth, fxtag, fx, sound, parent )
{
    if ( !isdefined( self gettagorigin( tag, 1 ) ) )
        return;
    part = spawnstruct();
    part.tag = tag;
    part.visible = tag;
    part.stage = 0;
    part.health = health;
    part.dhealth = dhealth;
    part.fx = fx;
    part.sound = sound;
    part.parent = parent;
    if ( isdefined( dtag ) && isdefined( self gettagorigin( dtag, 1 ) ) )
    {
        part.dtag = dtag;
        self hidepart( dtag );
    }
    part.fxtag = tag;
    if ( isdefined( fxtag ) && isdefined( self gettagorigin( fxtag, 1 ) ) )
        part.fxtag = fxtag;
    self.mw120r_parts[tag] = part;
}

// The part whose current bind-pose box (iw3compat_model_parts(), model space)
// holds the hit point; the smallest box wins, so glass beats its door.
iw3compat_vehicle_hit_part( point )
{
    if ( !isdefined( point ) || !isdefined( self.mw120r_part_min ) )
        return undefined;
    delta = point - self.origin;
    x = vectordot( delta, anglestoforward( self.angles ) );
    y = 0 - vectordot( delta, anglestoright( self.angles ) );
    z = vectordot( delta, anglestoup( self.angles ) );
    best = undefined;
    bestsize = 0;
    tags = getarraykeys( self.mw120r_parts );
    foreach ( tag in tags )
    {
        visible = self.mw120r_parts[tag].visible;
        if ( !isdefined( visible ) || !isdefined( self.mw120r_part_min[visible] ) )
            continue;
        lo = self.mw120r_part_min[visible] - ( 2, 2, 2 );
        hi = self.mw120r_part_max[visible] + ( 2, 2, 2 );
        if ( x < lo[0] || y < lo[1] || z < lo[2] || x > hi[0] || y > hi[1] || z > hi[2] )
            continue;
        size = ( hi[0] - lo[0] ) * ( hi[1] - lo[1] ) * ( hi[2] - lo[2] );
        if ( !isdefined( best ) || size < bestsize )
        {
            best = tag;
            bestsize = size;
        }
    }
    return best;
}

// IW3 destructible_update_part(): returns the damage passed on to the body.
// Melee and impacts break a part outright; leftover damage carries into the
// next state.
iw3compat_vehicle_damage_part( tag, amount, kind )
{
    part = self.mw120r_parts[tag];
    parent = 0;
    if ( kind != "splash" && isdefined( part.parent ) )
        parent = int( amount * part.parent );
    if ( !isdefined( part.health ) || part.stage >= 2 )
        return parent;
    if ( kind == "melee" || kind == "impact" )
        amount = 100000;
    while ( part.stage < 2 )
    {
        part.health = part.health - amount;
        if ( part.health > 0 )
            break;
        amount = 0 - part.health;
        self iw3compat_vehicle_break_part( part );
    }
    return parent;
}

iw3compat_vehicle_break_part( part )
{
    if ( part.stage == 0 )
    {
        self hidepart( part.tag );
        part.visible = undefined;
        if ( isdefined( part.dtag ) )
        {
            self showpart( part.dtag );
            part.visible = part.dtag;
            if ( isdefined( part.dhealth ) )
            {
                part.stage = 1;
                part.health = part.dhealth;
                return;
            }
        }
    }
    else
    {
        self hidepart( part.dtag );
        part.visible = undefined;
    }
    part.stage = 2;
    if ( isdefined( part.fx ) )
    {
        fx = level.mw120r_fx["glass_shatter"];
        if ( iw3compat_has_iw3fx( part.fx ) )
            fx = level.mw120r_iw3fx[part.fx];
        // IW3 bolts this to the tag; a light's tag is hidden at the same time,
        // so play it once at the tag's pose instead.
        angles = self gettagangles( part.fxtag );
        playfx( fx, self gettagorigin( part.fxtag ), anglestoforward( angles ), anglestoup( angles ) );
    }
    if ( isdefined( part.sound ) )
        iw3compat_playsoundatpos( self gettagorigin( part.fxtag ), part.sound );
}

// IW3 destructible_splash_damage(): every intact part takes the splash less
// 1.4 * its distance beyond the nearest part (the body counts, at its origin).
iw3compat_vehicle_parts_splash( amount, point )
{
    if ( !isdefined( point ) || !isdefined( amount ) || amount <= 0 )
        return;
    damage = amount * 2.75;
    closest = distance( point, self.origin );
    distances = [];
    tags = getarraykeys( self.mw120r_parts );
    foreach ( tag in tags )
    {
        visible = self.mw120r_parts[tag].visible;
        if ( !isdefined( visible ) )
            continue;
        distances[tag] = distance( point, self gettagorigin( visible ) );
        if ( distances[tag] < closest )
            closest = distances[tag];
    }
    foreach ( tag in tags )
    {
        if ( !isdefined( distances[tag] ) )
            continue;
        hit = damage - ( distances[tag] * 1.4 - closest );
        if ( hit > 0 )
            self iw3compat_vehicle_damage_part( tag, hit, "splash" );
    }
}

// ---------------------------------------------------------------------------
// IW3 maps\mp\_destructables: brush/model pieces removed after damage.
// ---------------------------------------------------------------------------
iw3compat_destructables()
{
    pieces = getentarray( "destructable", "targetname" );
    foreach ( piece in pieces )
        piece thread iw3compat_destructable_think();
}

iw3compat_destructable_think()
{
    self endon( "death" );
    self iw3compat_make_damageable();
    total = 0;
    for (;;)
    {
        self waittill( "damage", amount );
        total = total + amount;
        if ( total >= 40 )
            break;
    }

    playfx( level.mw120r_fx["sparks"], self.origin + ( 0, 0, 6 ) );
    self delete();
}

// ---------------------------------------------------------------------------
// Interactive TVs (IW3 maps\_interactive_objects): the first hit breaks the
// screen with glass and sparks.
// ---------------------------------------------------------------------------
iw3compat_interactive_tvs()
{
    tvs = getentarray( "mw120r_interactive_tv", "targetname" );
    foreach ( tv in tvs )
        tv thread iw3compat_tv_think();
}

iw3compat_tv_think()
{
    self endon( "death" );
    self iw3compat_make_damageable();
    if ( isdefined( self.model ) )
        self.mw120r_destroyed_model = iw3compat_known_model( self.model + "_d" );
    self waittill( "damage", amount, attacker, direction, point );
    self setcandamage( 0 );
    // The screen bone (IW3 tag_fx) when the model has it, else the hit point.
    screen = self gettagorigin( "tag_fx", 1 );
    if ( isdefined( screen ) )
        point = screen;
    if ( !isdefined( point ) )
        point = self.origin + ( 0, 0, 16 );

    playfx( level.mw120r_fx["glass_shatter"], point );
    playfx( level.mw120r_fx["sparks"], point );
    if ( isdefined( self.mw120r_destroyed_model ) )
        self setmodel( self.mw120r_destroyed_model );
}
