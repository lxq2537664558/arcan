-- image_tesselation
-- @short: alter the tesselation level of a vid
-- @inargs: vid:id,
-- @inargs: vid:id, func:callback
-- @inargs: vid:id, float:s, float:t
-- @inargs: vid:id, float:s, float:t, func:callback
-- @outargs:
-- @longdescr: This function is used to convert a normal vid to a pseudo-3D
-- object in the sense that it becomes a tesselated quad with *s* steps in one
-- dimension, and *t* steps in the other.
--
-- This is intended for very specific effects, either by itself or when
-- combined with a vertex shader stage. It is also usable for when you need to
-- show a sparsely populated area efficiently as a point-cloud but without the
-- setup of a 3D pipeline and a rendertarget.
--
-- By setting *s* or *t* to 1, tesselation is disabled and the underlying mesh
-- shape is deallocated. Updating the mesh shape to *s* > 1 and *t* > 1 will
-- regenerate and repopulate the individual attributes for vertices and texture
-- coordinates.
--
-- The vertices will be distributed in the -1..1 range (-1,-1 in upper left
-- corner, 1,1 in lower right) while the texture coordinates go in the 0..1
-- range with 0,0 at the upper left corner.
--
-- The storage can be accessed through the optional *callback* and has the
-- signature of (refobj, n_vertices, vertex_size) where *refobj* is a userdata
-- table that supports:
--
--  :vertices(ind) => x, y, z, w
--  :vertices(ind, x, y, z, w) => nil
--  for fetching and updating, with xyzw matching vertex_size
--  (=2, only xy, =3 only xyz, => w,y,z).
--
--  :colors(ind) => r, g, b, a
--  :colors(ind, new_r, new_g, new_b, new_a)
--  for fetching, activating and updating vertex colors.
--
--  :texcos(ind, set_ind) => s, t
--  :texcos(ind, set_ind, new_s, new_t)
--  for fetching and updating texture coordinates, set_ind should be 0 or 1
--  also aliased as texture_coordinates
--
--  :primitive_type(0 or 1) for switching between triangle soup (0)
--  and point-cloud (1) when rendering.
--
-- If any of these storage blocks doesn't exist, an attempt to set an index
-- will force it to be allocated.
--
-- @note: Picking operations like image_hit does not take tesselation into
-- account. This may be subject to change.
--
-- @group: image
-- @cfunction: imagetess
-- @related:
-- @flags: experimental
function main()
#ifdef MAIN
	local tess = fill_surface(64, 64, 0, 255, 0);
	show_image(tess);
	image_tesselation(tess, 32, 32,
		function(obj, n_v, size)
			obj:primitive_type(1);
		end
	);
#endif

#ifdef ERROR1
#endif
end
