--
-- helper functions shared by all the other gridle subsystems
-- these could easily be generalized / translated to work for other themes as well
--
-- dependencies: listview.lua, osdkbd.lua, dialog.lua, colourtable.lua
-- globals: "settings" table to manipulate, keyconfig in place, "imagery" table to manipulate, soundmap for soundeffects associated with labels
-- gridle_input (dispatch routines override the default input handler)
--
-- one of the many string functions missing from the std lua library, just drop trailing or leading whitespaces
function string.trim(s)
	return (s:gsub("^%s*(.-)%s*$", "%1"))
end

function table.find(table, label)
	for a,b in pairs(table) do
		if (b == label) then return a end
	end

	return nil;  
end

function string.split(instr, delim)
	local res = {};
	local strt = 1;
	local delim_pos, delim_stp = string.find(instr, delim, strt);
	
	while delim_pos do
		table.insert(res, string.sub(instr, strt, delim_pos-1));
		strt = delim_stp + 1;
		delim_pos, delim_stp = string.find(instr, delim, strt);
	end
	
	table.insert(res, string.sub(instr, strt));
	return res;
end

-- (hidden) local dispatch function that gets used when someone pushes a table but no name 
local function dispatch_input(iotbl, override)
	local restbl = keyconfig:match(iotbl);
	
	if (restbl and (iotbl.active or iotbl.kind == "analog")) then
		for ind,val in pairs(restbl) do

			if (settings.iodispatch[val]) then
				settings.iodispatch[val](restbl, iotbl);
			end
		end
	end

end

--
-- replace the current input handling routine with the specified triggerfun (dispatchinput if nil)
-- with the specified dispatch table (tbl)
-- 'name' is used to assist tracing/debugging
-- 
function dispatch_push(tbl, name, triggerfun)
	if (settings.dispatch_stack == nil) then
		settings.dispatch_stack = {};
	end

	local newtbl = {};

	newtbl.table = tbl;
	newtbl.name = name;
	newtbl.dispfun = triggerfun and triggerfun or dispatch_input;
	
	table.insert(settings.dispatch_stack, newtbl);
	settings.iodispatch = tbl;

	local input_key = string.lower(THEMENAME) .. "_input";
	_G[input_key] = newtbl.dispfun;
	
	print("push:", tostring(tbl));
	for ind, val in ipairs(settings.dispatch_stack) do
		print(val.name);
	end
	print("/push")
end

function dispatch_pop()
	if (#settings.dispatch_stack <= 1) then
		gridle_input = gridle_dispatchinput;
		return "";
	else
		table.remove(settings.dispatch_stack, #settings.dispatch_stack);
		local last = settings.dispatch_stack[#settings.dispatch_stack];

		settings.iodispatch = last.table;
		gridle_input = last.dispfun;
		print("pop to: ", last.name);
		return last.name;
	end
end

function spawn_warning( message, persist )
-- render message and make sure it is on top
	local msg = {};
	local exptime = 100;

	if (type(message) == "table") then
		exptime = exptime * #message;
		msg = message;
	else
		local msgstr = string.gsub(message, "\\", "\\\\");
		table.insert(msg, msgstr);
	end
	
	local infowin     = listview_create( msg, VRESW / 2, VRESH / 2 );
	infowin:show();

	local x = math.floor( 0.5 * (VRESW - image_surface_properties(infowin.border, 100).width)  );
	local y = math.floor( 0.5 * (VRESH - image_surface_properties(infowin.border, 100).height) );

	move_image(infowin.anchor, x, y);
	hide_image(infowin.cursorvid);
	if (persist == nil or persist == false) then
		expire_image(infowin.anchor, exptime);
		blend_image(infowin.window, 1.0, 50);
		blend_image(infowin.border, 1.0, 50);
		blend_image(infowin.window, 0.0, exptime - 50 - 25);
		blend_image(infowin.border, 0.0, exptime - 50 - 25);
	end

	return infowin;
end

-- Spawn a modal dialog window displaying [message (stringtable or string, \ will be escaped to \\)],
-- letting the user to select between [buttons (stringtable)]. valbls[chosen button] will be triggered upon selection,
-- cleanuphook() called (unless nil) when user has either selected or pressed escape
-- and canescape (boolean) determines if the user is allowed to use MENU_ESCAPE or not.
function dialog_option( message, buttons, canescape, valcbs, cleanuphook )
	local dialogwin = dialog_create(message, buttons);
	local imenu = {};
	
	imenu["MENU_LEFT"] = function()
		dialogwin:input("MENU_LEFT");
		play_audio(soundmap["GRIDCURSOR_MOVE"]);
	end

	imenu["MENU_RIGHT"] = function()
		dialogwin:input("MENU_RIGHT");
		play_audio(soundmap["GRIDCURSOR_MOVE"]);
	end

	imenu["MENU_SELECT"] = function()
		local res = dialogwin:input("MENU_SELECT");
		play_audio(soundmap["MENU_SELECT"]);
		dispatch_pop();
		if (valcbs and valcbs[res]) then valcbs[res](); end
		if (cleanuphook) then cleanuphook(); end
	end

	if (canescape) then
		imenu["MENU_ESCAPE"] = function()
			dialogwin:input("MENU_ESCAPE");
			play_audio(soundmap["MENU_FADE"]);
			if (cleanuphook) then cleanuphook(); end
			dispatch_pop();
		end
	end

	play_audio(soundmap["MENU_TOGGLE"]);
	dispatch_push(imenu, "message dialog");
	dialogwin:show();
end

-- Ask the user if he wants to shut down the program or not, disables 3D models while doing so.
function confirm_shutdown()
	local valcbs = {};

	valcbs["YES"] = function()
		shutdown();
	end

	video_3dorder(ORDER_NONE);
	kbd_repeat(0);

	dialog_option(settings.colourtable.fontstr .. "Shutdown Arcan/Gridle?", {"NO", "YES"}, true, valcbs, function()
		kbd_repeat(settings.repeatrate);
		video_3dorder(ORDER_LAST);
	end);
end

-- Locate "playlist" in 'music/playlists/' and populate the global settings.playlist with all entries that doesn't begin with #
function music_load_playlist(playlist)
	settings.playlist = {};

	if (playlist == nil or string.len(playlist) == 0) then
		return false;
	end

	if (open_rawresource("music/playlists/" .. playlist)) then
		local line = read_rawresource();
		
		while line do
			local ch = string.sub(line, 1, 1);

			if (ch ~= "" and ch ~= "#") then
				table.insert(settings.playlist, line);
			end
			line = read_rawresource();
		end

		close_rawresource();
		return true;
	end
end

-- Take the global settings.playlist
function music_randomize_playlist()
	local newlist = {};

	while #settings.playlist > 0 do
		local entry = table.remove(settings.playlist, math.random(1, #settings.playlist));
	
		if (entry) then
			table.insert(newlist, entry);
		end
	end

	settings.playlist = newlist;
	settings.playlist_ofs = 1;
end

-- Recursively increment the settings.playlist_ofs and look for matching resources (playlists should
-- be relative the music or music/playlists folder) until a matching resource is found or reccount
-- amount of times exceeds the reccount tries. 
function music_next_song(reccount)
	if (reccount == 0) then
		spawn_warning("Music Player: Couldn't find next track to play, empty or broken playlist?");
		return nil;
	end
	
	settings.playlist_ofs = settings.playlist_ofs + 1;

	if (settings.playlist_ofs > #settings.playlist) then
		settings.playlist_ofs = 1;
	end

	local label = settings.playlist[settings.playlist_ofs];
	if (resource(label)) then
		return label;
	elseif (resource("music/" .. label)) then
		return "music/" .. label;
	elseif (resource("music/playlists/" .. label)) then
		return "music/playlists/" .. label;
	end

	return gridle_nextsong(reccount - 1);
end

-- create a persistant frameserver connection responsible for streaming a song,
-- whenever that finishes, traverse the playlist to find a new one etc.
function music_start_bgmusic(playlist)
	if (not music_load_playlist(playlist) or settings.bgmusic == "Disabled" ) then
		return;
	end

	settings.playlist_ofs = 1;
	if (settings.bgmusic_order == "Randomized") then
		music_randomize_playlist();
	end

	if (valid_vid(imagery.musicplayer)) then
		delete_image(imagery.musicplayer);
	end

-- the chainloading stream trigger
	local function musicplayer_trigger(source, status)
		if (status.kind == "frameserver_terminated") then
			delete_image(imagery.musicplayer);

			local song = music_next_song( #settings.playlist );
			if (song == nil) then
				imagery.musicplayer = nil;
			end

			imagery.musicplayer = load_movie(song,  FRAMESERVER_NOLOOP, musicplayer_trigger);
			persist_image(imagery.musicplayer);
			image_tracetag(imagery.musicplayer, "music player");
		
		elseif (status.kind == "resized") then
			audio_gain(status.source_audio, 0.0);
			audio_gain(status.source_audio, settings.bgmusic_gain, settings.transitiondelay);
			play_movie(source);
		end
	end

-- similar to chainloading, but bootstrap
	if (#settings.playlist > 0) then
		local song = music_next_song( #settings.playlist );
		if (song ~= nil) then
			settings.playlist_ofs = 1;
			imagery.musicplayer = load_movie(song, FRAMESERVER_NOLOOP, musicplayer_trigger);
			persist_image(imagery.musicplayer);
			image_tracetag(imagery.musicplayer, "music player");
		end
	end

end

--
-- Menu functions
-- all these are centered around the behaviours of the listview.lua script,
-- so we have three tables per menu, (labels, pointers and styleprefix)
--

--
-- generate a list of menuentries based on the globpattern (globstr) in the resource category globmask (_THEME, _SHARED, _ALL)
-- where MENU_SELECT would trigger cbfun
--
function build_globmenu(globstr, cbfun, globmask)
	local lists = glob_resource(globstr, globmask);
	local resptr = {};
	
	for i = 1, #lists do
		resptr[ lists[i] ] = cbfun;
	end
	
	return lists, resptr;
end

--
-- Wrapper around build_globmenu that takes all the "saving settings etc." into account as well
-- name: is the settings and k/v store key to use for saving settings and getting current value
-- globstr: pattern to look for
-- globmask: which resource category to target
-- triggerfun: invoke when a globbed entry is selected
-- failtrig: invoked when globbing didn't return any results
--
function gen_glob_menu(name, globstr, globmask, triggerfun, failtrig)
	local resptr = {};

	local togglefun = function(label, save)
		settings.iodispatch["MENU_ESCAPE"](nil, nil, true);
		if (name ~= nil) then
			settings[name] = label;
		end

		if (save and name ~= nil) then
			play_audio(soundmap["MENU_FAVORITE"]);
			store_key(name, label);
		else
			play_audio(soundmap["MENU_SELECT"]);
		end

		if (triggerfun) then
			triggerfun(label);
		end
	end
	
	local lists = glob_resource(globstr, globmask);
	if (lists == nil or #lists == 0) then
		if (failtrig ~= nil) then
			failtrig();
		end
		return;
	end

	for i = 1, #lists do
		resptr[lists[i]] = togglefun;
	end

	return lists, resptr;
end

-- 
-- Take a table of values and generate functions etc. to match the entries
-- name     : settings[name] to store under
-- tbl      : array of string with labels of possible values
-- trggerfun: when selected, this function will be called (useful for activating whatever setting changed)
-- isstring : treat value as string or convert to number before sending to store_key
--
function gen_tbl_menu(name, tbl, triggerfun, isstring)
	local reslbl = {};
	local resptr = {};

	local basename = function(label, save)
		settings.iodispatch["MENU_ESCAPE"](nil, nil, true);
		if (name ~= nil) then
			settings[name] = isstring and label or tonumber(label);
		end

		if (save and name ~= nil) then
			play_audio(soundmap["MENU_FAVORITE"]);
			store_key(name, isstring and label or tonumber(label));
		else
			play_audio(soundmap["MENU_SELECT"]);
		end

		if (triggerfun) then triggerfun(label); end
	end

	for key,val in ipairs(tbl) do
		table.insert(reslbl, val);
		resptr[val] = basename;
	end

	return reslbl, resptr;
end

-- automatically generate a menu of numbers
-- name  : the settings key to store in
-- base  : start value
-- step  : value to add to base, or a function that calculates the value using an index
-- count : number of entries
-- triggerfun : hook to be called when selected 
function gen_num_menu(name, base, step, count, triggerfun)
	local reslbl = {};
	local resptr = {};
	local clbl = base;
	
	local basename = function(label, save)
		settings.iodispatch["MENU_ESCAPE"](nil, nil, true);
		settings[name] = tonumber(label);
		if (save) then
			play_audio(soundmap["MENU_FAVORITE"]);
			store_key(name, tonumber(label));
		else
			play_audio(soundmap["MENU_SELECT"]);
		end
		
		if (triggerfun) then triggerfun(); end
	end

	clbl = base;
	for i=1,count do
		if (type(step) == "function") then 
			clbl = step(i); 
			if (clbl == nil) then 
				break;
			end
		end

		table.insert(reslbl, tostring(clbl));
		resptr[tostring(clbl)] = basename;
		
		if (type(step) == "number") then clbl = clbl + step; end
	end

	return reslbl, resptr;
end

-- inject a submenu into a main one
-- dstlbls : array of strings to insert into
-- dstptrs : hashtable keyed by label for which to insert the spawn function
-- label   : to use for dstlbls/dstptrs
-- lbls    : array of strings used in the submenu (typically from gen_num, gen_tbl)
-- ptrs    : hashtable keyed by label that acts as triggers (typically from gen_num, gen_tbl)
function add_submenu(dstlbls, dstptrs, label, key, lbls, ptrs, fmt)
	if (dstlbls == nil or dstptrs == nil or lbls == nil or #lbls == 0) then return; end

	if (not table.find(dstlbls, label)) then
		table.insert(dstlbls, label);
	end
	
	dstptrs[label] = function()
		local fmts = {};
		
		if (key ~= nil) then
			local ind = tostring(settings[key]);

			if (fmt ~= nil) then
				fmts = fmt;
			elseif (ind) then
				fmts[ ind ] = settings.colourtable.notice_fontstr;
				if(get_key(key)) then
					fmts[ get_key(key) ] = settings.colourtable.alert_fontstr;
				end
			end
		end
		
		menu_spawnmenu(lbls, ptrs, fmts);
	end -- of function
end

-- create and display a listview setup with the menu defined by the arguments.
-- list    : array of strings that make up the menu
-- listptr : hashtable keyed by list labels
-- fmtlist : hashtable keyed by list labels, on match, will be prepended when rendering (used for icons, highlights etc.)
function menu_spawnmenu(list, listptr, fmtlist)
	if (#list < 1) then
		return nil;
	end

	local parent = current_menu;
	local props = image_surface_resolve_properties(current_menu.cursorvid);
	local windsize = VRESH;

	local yofs = 0;
	if (props.y + windsize > VRESH) then
		yofs = VRESH - windsize;
	end

	current_menu = listview_create(list, windsize, VRESW / 3, fmtlist);
	current_menu.parent = parent;
	current_menu.ptrs = listptr;
	current_menu.updatecb = parent.updatecb;
	current_menu:show();
	move_image( current_menu.anchor, props.x + props.width + 6, props.y);
	
	local xofs = 0;
	local yofs = 0;
	
-- figure out where the window is going to be.
	local aprops_l = image_surface_properties(current_menu.anchor, settings.fadedelay);
	local wprops_l = image_surface_properties(current_menu.window, settings.fadedelay);
	local dx = aprops_l.x;
	local dy = aprops_l.y;
	
	local winw = wprops_l.width;
	local winh = wprops_l.height;
	
	if (dx + winw > VRESW) then
		dx = dx + (VRESW - (dx + winw));
	end
	
	if (dy + winh > VRESH) then
		dy = dy + (VRESH - (dy + winh));
	end

	move_image( current_menu.anchor, math.floor(dx), math.floor(dy), settings.fadedelay );
	
	play_audio(soundmap["SUBMENU_TOGGLE"]);
	return current_menu;
end

-- default handler that sets up all shared members etc. needed for gridle_internal functions,
-- used by both custom, detail and grid view.
function internallaunch_event(source, datatbl)
	if (datatbl.kind == "resized") then
		if (not settings.in_internal) then
-- remap input function to one that can handle forwarding and have access to context specific menu
			dispatch_push(settings.iodispatch, "internal_input", gridle_internalinput);
		end

		gridle_internal_setup(source, datatbl, current_game);

	elseif (datatbl.kind == "frameserver_terminated") then
		if (settings.status_loading) then
			remove_loaded();
			dispatch_pop();
		end
	
		order_image(imagery.crashimage, INGAMELAYER_OVERLAY);
		blend_image(imagery.crashimage, 0.8);

		if (not settings.in_internal) then
			blend_image(imagery.crashimage, 0.0, settings.fadedelay + 10);
		end

	elseif (datatbl.kind == "message") then
		spawn_warning(datatbl.message);

	elseif (datatbl.kind == "ident") then
		settings.internal_ident = datatbl.message;
		
	elseif (datatbl.kind == "state_size") then
		if (datatbl.state_size <= 0) then
			disable_snapshot();
		end

	elseif (datatbl.kind == "frame") then
-- just ignore

	elseif (datatbl.kind == "resource_status") then
		if (datatbl.message == "loading") then
			show_loading();
			spawn_warning(settings.internal_ident);
		elseif( datatbl.message == "loaded" or "failed") then
			remove_loaded();
		end
	end

end

--
-- populate a dispatch table for working with a current_menu global
-- will not override currently defined symbols
--
function menu_defaultdispatch(dst)
	if (not dst["MENU_UP"]) then
		dst["MENU_UP"] = function(iotbl)
			play_audio(soundmap["GRIDCURSOR_MOVE"]);
			current_menu:move_cursor(-1, true); 
		end
	end

	if (not dst["MENU_DOWN"]) then
			dst["MENU_DOWN"] = function(iotbl)
			play_audio(soundmap["GRIDCURSOR_MOVE"]);
			current_menu:move_cursor(1, true); 
		end
	end
	
	if (not dst["MENU_SELECT"]) then
		dst["MENU_SELECT"] = function(iotbl)
			selectlbl = current_menu:select();
			if (current_menu.ptrs[selectlbl]) then
				current_menu.ptrs[selectlbl](selectlbl, false);
				if (current_menu and current_menu.updatecb) then
					current_menu.updatecb();
				end
			end
		end
	end
	
-- figure out if we should modify the settings table
	if (not dst["FLAG_FAVORITE"]) then
		dst["FLAG_FAVORITE"] = function(iotbl)
				selectlbl = current_menu:select();
				if (current_menu.ptrs[selectlbl]) then
					current_menu.ptrs[selectlbl](selectlbl, true);
					if (current_menu and current_menu.updatecb) then
						current_menu.updatecb();
					end
				end
			end
	end
	
	if (not dst["MENU_ESCAPE"]) then
		dst["MENU_ESCAPE"] = function(iotbl, restbl, silent)
			current_menu:destroy();
			if (current_menu.parent ~= nil) then
				if (silent == nil or silent == false) then play_audio(soundmap["SUBMENU_FADE"]); end
				current_menu = current_menu.parent;
			else -- top level
				play_audio(soundmap["MENU_FADE"]);
				dispatch_pop();
			end
		end
	end
	
	if (not dst["MENU_RIGHT"]) then
		dst["MENU_RIGHT"] = dst["MENU_SELECT"];
	end
	
	if (not dst["MENU_LEFT"]) then
		dst["MENU_LEFT"]  = dst["MENU_ESCAPE"];
	end
end