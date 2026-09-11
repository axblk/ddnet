/* The font formats this game opens, and no others.
 *
 * FreeType registers every driver it was built with in a list that its
 * initialisation walks, so a format nobody will ever be handed is still
 * linked in - the bitmap formats of the nineties, Type 1 and its relatives,
 * the SVG and signed-distance-field renderers. This file is that list,
 * handed to FreeType as `FT_CONFIG_MODULES_H`, cut down to what
 * `data/fonts` and anything a player is plausibly going to point at
 * actually is: TrueType, OpenType, and the two of them in a collection.
 *
 * The drivers left out are still compiled; the linker drops them because
 * nothing names them any more.
 */

/* TrueType, and the tables shared with OpenType. */
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
/* OpenType, whose outlines are CFF and whose charstrings are read by
   `psaux`, named by `psnames` and hinted by `pshinter`. */
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
/* The hinter for the fonts that bring no hints of their own. */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
/* Glyphs come out of `FT_LOAD_RENDER` anti-aliased, which is this one
   renderer; the game never asks for a black and white bitmap, a distance
   field or an SVG. */
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
