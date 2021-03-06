/* Aseprite
 * Copyright (C) 2001-2014  David Capello
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/document_api.h"

#include "app/document.h"
#include "app/document_undo.h"
#include "app/undoers/add_cel.h"
#include "app/undoers/add_frame.h"
#include "app/undoers/add_image.h"
#include "app/undoers/add_layer.h"
#include "app/undoers/add_palette.h"
#include "app/undoers/dirty_area.h"
#include "app/undoers/flip_image.h"
#include "app/undoers/image_area.h"
#include "app/undoers/move_layer.h"
#include "app/undoers/move_layer.h"
#include "app/undoers/remove_cel.h"
#include "app/undoers/remove_frame.h"
#include "app/undoers/remove_image.h"
#include "app/undoers/remove_layer.h"
#include "app/undoers/remove_palette.h"
#include "app/undoers/replace_image.h"
#include "app/undoers/set_cel_frame.h"
#include "app/undoers/set_cel_opacity.h"
#include "app/undoers/set_cel_position.h"
#include "app/undoers/set_frame_duration.h"
#include "app/undoers/set_layer_flags.h"
#include "app/undoers/set_layer_flags.h"
#include "app/undoers/set_layer_name.h"
#include "app/undoers/set_layer_name.h"
#include "app/undoers/set_mask.h"
#include "app/undoers/set_mask_position.h"
#include "app/undoers/set_palette_colors.h"
#include "app/undoers/set_sprite_pixel_format.h"
#include "app/undoers/set_sprite_size.h"
#include "app/undoers/set_sprite_transparent_color.h"
#include "app/undoers/set_total_frames.h"
#include "base/unique_ptr.h"
#include "doc/document_event.h"
#include "doc/document_observer.h"
#include "raster/algorithm/flip_image.h"
#include "raster/algorithm/shrink_bounds.h"
#include "raster/blend.h"
#include "raster/cel.h"
#include "raster/dirty.h"
#include "raster/image.h"
#include "raster/image_bits.h"
#include "raster/layer.h"
#include "raster/mask.h"
#include "raster/palette.h"
#include "raster/quantization.h"
#include "raster/sprite.h"
#include "raster/stock.h"


namespace app {

DocumentApi::DocumentApi(Document* document, undo::UndoersCollector* undoers)
  : m_document(document)
  , m_undoers(undoers)
{
}

undo::ObjectsContainer* DocumentApi::getObjects() const
{
  return m_document->getUndo()->getObjects();
}

void DocumentApi::setSpriteSize(Sprite* sprite, int w, int h)
{
  ASSERT(w > 0);
  ASSERT(h > 0);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetSpriteSize(getObjects(), sprite));

  sprite->setSize(w, h);

  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onSpriteSizeChanged, ev);
}

void DocumentApi::setSpriteTransparentColor(Sprite* sprite, color_t maskColor)
{
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetSpriteTransparentColor(getObjects(), sprite));

  sprite->setTransparentColor(maskColor);

  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onSpriteTransparentColorChanged, ev);
}

void DocumentApi::cropSprite(Sprite* sprite, const gfx::Rect& bounds, color_t bgcolor)
{
  setSpriteSize(sprite, bounds.w, bounds.h);
  displaceLayers(sprite->folder(), -bounds.x, -bounds.y);

  Layer *background_layer = sprite->backgroundLayer();
  if (background_layer)
    cropLayer(background_layer, 0, 0, sprite->width(), sprite->height(), bgcolor);

  if (!m_document->mask()->isEmpty())
    setMaskPosition(m_document->mask()->bounds().x-bounds.x,
                    m_document->mask()->bounds().y-bounds.y);
}

void DocumentApi::trimSprite(Sprite* sprite, color_t bgcolor)
{
  gfx::Rect bounds;

  base::UniquePtr<Image> image_wrap(Image::create(sprite->pixelFormat(),
                                                  sprite->width(),
                                                  sprite->height()));
  Image* image = image_wrap.get();

  for (FrameNumber frame(0); frame<sprite->totalFrames(); ++frame) {
    image->clear(0);

    sprite->render(image, 0, 0, frame);

    // TODO configurable (what color pixel to use as "refpixel",
    // here we are using the top-left pixel by default)
    gfx::Rect frameBounds;
    if (raster::algorithm::shrink_bounds(image, frameBounds, get_pixel(image, 0, 0)))
      bounds = bounds.createUnion(frameBounds);
  }

  if (!bounds.isEmpty())
    cropSprite(sprite, bounds, bgcolor);
}

void DocumentApi::setPixelFormat(Sprite* sprite, PixelFormat newFormat, DitheringMethod dithering_method)
{
  Image* old_image;
  Image* new_image;
  int c;

  if (sprite->pixelFormat() == newFormat)
    return;

  // TODO Review this, why we use the palette in frame 0?
  FrameNumber frame(0);

  // Use the rgbmap for the specified sprite
  const RgbMap* rgbmap = sprite->getRgbMap(frame);

  // Get the list of cels from the background layer (if it
  // exists). This list will be used to check if each image belong to
  // the background layer.
  CelList bgCels;
  if (sprite->backgroundLayer() != NULL)
    sprite->backgroundLayer()->getCels(bgCels);

  for (c=0; c<sprite->stock()->size(); c++) {
    old_image = sprite->stock()->getImage(c);
    if (!old_image)
      continue;

    bool is_image_from_background = false;
    for (CelList::iterator it=bgCels.begin(), end=bgCels.end(); it != end; ++it) {
      if ((*it)->imageIndex() == c) {
        is_image_from_background = true;
        break;
      }
    }

    new_image = quantization::convert_pixel_format
      (old_image, NULL, newFormat, dithering_method, rgbmap,
       sprite->getPalette(frame),
       is_image_from_background);

    replaceStockImage(sprite, c, new_image);
  }

  // Set all cels opacity to 100% if we are converting to indexed.
  if (newFormat == IMAGE_INDEXED) {
    CelList cels;
    sprite->getCels(cels);
    for (CelIterator it = cels.begin(), end = cels.end(); it != end; ++it) {
      Cel* cel = *it;
      if (cel->opacity() < 255)
        setCelOpacity(sprite, *it, 255);
    }
  }

  // Change sprite's pixel format.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetSpritePixelFormat(getObjects(), sprite));

  sprite->setPixelFormat(newFormat);

  // Regenerate extras
  m_document->destroyExtraCel();

  // When we are converting to grayscale color mode, we've to destroy
  // all palettes and put only one grayscaled-palette at the first
  // frame.
  if (newFormat == IMAGE_GRAYSCALE) {
    // Add undoers to revert all palette changes.
    if (undoEnabled()) {
      PalettesList palettes = sprite->getPalettes();
      for (PalettesList::iterator it = palettes.begin(); it != palettes.end(); ++it) {
        Palette* palette = *it;
        m_undoers->pushUndoer(new undoers::RemovePalette(
            getObjects(), sprite, palette->frame()));
      }

      m_undoers->pushUndoer(new undoers::AddPalette(
        getObjects(), sprite, FrameNumber(0)));
    }

    // It's a base::UniquePtr because setPalette'll create a copy of "graypal".
    base::UniquePtr<Palette> graypal(Palette::createGrayscale());

    sprite->resetPalettes();
    sprite->setPalette(graypal, true);
  }
}

void DocumentApi::addFrame(Sprite* sprite, FrameNumber newFrame)
{
  copyFrame(sprite, newFrame.previous(), newFrame);
}

void DocumentApi::addEmptyFrame(Sprite* sprite, FrameNumber newFrame, color_t bgcolor)
{
  // Add the frame in the sprite structure, it adjusts the total
  // number of frames in the sprite.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::AddFrame(getObjects(), m_document, sprite, newFrame));

  sprite->addFrame(newFrame);

  // Move cels.
  displaceFrames(sprite->folder(), newFrame);

  // Add background cel
  Layer* bgLayer = sprite->backgroundLayer();
  if (bgLayer) {
    LayerImage* imglayer = static_cast<LayerImage*>(bgLayer);
    Image* bgimage = Image::create(sprite->pixelFormat(), sprite->width(), sprite->height());
    clear_image(bgimage, bgcolor);
    addImage(imglayer, newFrame, bgimage);
  }

  // Notify observers about the new frame.
  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.frame(newFrame);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onAddFrame, ev);
}

void DocumentApi::copyFrame(Sprite* sprite, FrameNumber fromFrame, FrameNumber newFrame)
{
  // Add the frame in the sprite structure, it adjusts the total
  // number of frames in the sprite.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::AddFrame(getObjects(), m_document, sprite, newFrame));

  sprite->addFrame(newFrame);

  // Move cels, and create copies of the cels in the given "newFrame".
  copyFrameForLayer(sprite->folder(), fromFrame, newFrame);

  // Notify observers about the new frame.
  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.frame(newFrame);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onAddFrame, ev);
}

void DocumentApi::displaceFrames(Layer* layer, FrameNumber frame)
{
  ASSERT(layer);
  ASSERT(frame >= 0);

  Sprite* sprite = layer->sprite();

  switch (layer->type()) {

    case OBJECT_LAYER_IMAGE: {
      LayerImage* imglayer = static_cast<LayerImage*>(layer);

      // Displace all cels in '>=frame' to the next frame.
      for (FrameNumber c=sprite->lastFrame(); c>=frame; --c) {
        Cel* cel = imglayer->getCel(c);
        if (cel)
          setCelFramePosition(imglayer, cel, cel->frame().next());
      }
      break;
    }

    case OBJECT_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->getLayerBegin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->getLayerEnd();

      for (; it != end; ++it)
        displaceFrames(*it, frame);
      break;
    }

  }
}

void DocumentApi::copyFrameForLayer(Layer* layer, FrameNumber fromFrame, FrameNumber frame)
{
  ASSERT(layer);
  ASSERT(frame >= 0);

  Sprite* sprite = layer->sprite();

  switch (layer->type()) {

    case OBJECT_LAYER_IMAGE: {
      LayerImage* imglayer = static_cast<LayerImage*>(layer);

      displaceFrames(imglayer, frame);

      if (fromFrame >= frame)
        fromFrame = fromFrame.next();

      copyCel(imglayer, fromFrame, imglayer, frame, 0);
      break;
    }

    case OBJECT_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->getLayerBegin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->getLayerEnd();

      for (; it != end; ++it)
        copyFrameForLayer(*it, fromFrame, frame);
      break;
    }

  }
}

void DocumentApi::removeFrame(Sprite* sprite, FrameNumber frame)
{
  ASSERT(frame >= 0);

  // Remove cels from this frame (and displace one position backward
  // all next frames)
  removeFrameOfLayer(sprite->folder(), frame);

  // Add undoers to restore the removed frame from the sprite (to
  // restore the number and durations of frames).
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::RemoveFrame(getObjects(), m_document, sprite, frame));

  // Remove the frame from the sprite. This is the low level
  // operation, it modifies the number and duration of frames.
  sprite->removeFrame(frame);

  // Notify observers.
  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.frame(frame);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onRemoveFrame, ev);
}

// Does the hard part of removing a frame: Removes all cels located in
// the given frame, and moves all following cels one frame position back.
void DocumentApi::removeFrameOfLayer(Layer* layer, FrameNumber frame)
{
  ASSERT(layer);
  ASSERT(frame >= 0);

  Sprite* sprite = layer->sprite();

  switch (layer->type()) {

    case OBJECT_LAYER_IMAGE: {
      LayerImage* imglayer = static_cast<LayerImage*>(layer);
      if (Cel* cel = imglayer->getCel(frame))
        removeCel(imglayer, cel);

      for (++frame; frame<sprite->totalFrames(); ++frame)
        if (Cel* cel = imglayer->getCel(frame))
          setCelFramePosition(imglayer, cel, cel->frame().previous());
      break;
    }

    case OBJECT_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->getLayerBegin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->getLayerEnd();

      for (; it != end; ++it)
        removeFrameOfLayer(*it, frame);
      break;
    }

  }
}

void DocumentApi::setTotalFrames(Sprite* sprite, FrameNumber frames)
{
  ASSERT(frames >= 1);

  // Add undoers.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetTotalFrames(getObjects(), m_document, sprite));

  // Do the action.
  sprite->setTotalFrames(frames);

  // Notify observers.
  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.frame(frames);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onTotalFramesChanged, ev);
}

void DocumentApi::setFrameDuration(Sprite* sprite, FrameNumber frame, int msecs)
{
  // Add undoers.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetFrameDuration(
        getObjects(), sprite, frame));

  // Do the action.
  sprite->setFrameDuration(frame, msecs);

  // Notify observers.
  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.frame(frame);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onFrameDurationChanged, ev);
}

void DocumentApi::setFrameRangeDuration(Sprite* sprite, FrameNumber from, FrameNumber to, int msecs)
{
  ASSERT(from >= FrameNumber(0));
  ASSERT(from < to);
  ASSERT(to <= sprite->lastFrame());

  // Add undoers.
  if (undoEnabled()) {
    for (FrameNumber fr(from); fr<=to; ++fr)
      m_undoers->pushUndoer(new undoers::SetFrameDuration(
          getObjects(), sprite, fr));
  }

  // Do the action.
  sprite->setFrameRangeDuration(from, to, msecs);
}

void DocumentApi::moveFrame(Sprite* sprite, FrameNumber frame, FrameNumber beforeFrame)
{
  if (frame != beforeFrame &&
      frame >= 0 &&
      frame <= sprite->lastFrame() &&
      beforeFrame >= 0 &&
      beforeFrame <= sprite->lastFrame().next()) {
    // Change the frame-lengths...
    int frlen_aux = sprite->getFrameDuration(frame);

    // Moving the frame to the future.
    if (frame < beforeFrame) {
      for (FrameNumber c=frame; c<beforeFrame.previous(); ++c)
        setFrameDuration(sprite, c, sprite->getFrameDuration(c.next()));

      setFrameDuration(sprite, beforeFrame.previous(), frlen_aux);
    }
    // Moving the frame to the past.
    else if (beforeFrame < frame) {
      for (FrameNumber c=frame; c>beforeFrame; --c)
        setFrameDuration(sprite, c, sprite->getFrameDuration(c.previous()));

      setFrameDuration(sprite, beforeFrame, frlen_aux);
    }

    // change the cels of position...
    moveFrameLayer(sprite->folder(), frame, beforeFrame);
  }
}

void DocumentApi::moveFrameLayer(Layer* layer, FrameNumber frame, FrameNumber beforeFrame)
{
  ASSERT(layer);

  switch (layer->type()) {

    case OBJECT_LAYER_IMAGE: {
      LayerImage* imglayer = static_cast<LayerImage*>(layer);

      CelList cels;
      imglayer->getCels(cels);

      CelIterator it = cels.begin();
      CelIterator end = cels.end();

      for (; it != end; ++it) {
        Cel* cel = *it;
        FrameNumber celFrame = cel->frame();
        FrameNumber newFrame = celFrame;

        // fthe frame to the future
        if (frame < beforeFrame) {
          if (celFrame == frame) {
            newFrame = beforeFrame.previous();
          }
          else if (celFrame > frame &&
                   celFrame < beforeFrame) {
            --newFrame;
          }
        }
        // moving the frame to the past
        else if (beforeFrame < frame) {
          if (celFrame == frame) {
            newFrame = beforeFrame;
          }
          else if (celFrame >= beforeFrame &&
                   celFrame < frame) {
            ++newFrame;
          }
        }

        if (celFrame != newFrame)
          setCelFramePosition(imglayer, cel, newFrame);
      }
      break;
    }

    case OBJECT_LAYER_FOLDER: {
      LayerIterator it = static_cast<LayerFolder*>(layer)->getLayerBegin();
      LayerIterator end = static_cast<LayerFolder*>(layer)->getLayerEnd();

      for (; it != end; ++it)
        moveFrameLayer(*it, frame, beforeFrame);
      break;
    }

  }
}

void DocumentApi::addCel(LayerImage* layer, Cel* cel)
{
  ASSERT(layer);
  ASSERT(cel);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::AddCel(getObjects(), layer, cel));

  layer->addCel(cel);

  doc::DocumentEvent ev(m_document);
  ev.sprite(layer->sprite());
  ev.layer(layer);
  ev.cel(cel);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onAddCel, ev);
}

void DocumentApi::removeCel(LayerImage* layer, Cel* cel)
{
  ASSERT(layer);
  ASSERT(cel);

  Sprite* sprite = layer->sprite();

  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.layer(layer);
  ev.cel(cel);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onRemoveCel, ev);

  // Find if the image that use this cel we are going to remove, is
  // used by other cels.
  size_t refs = sprite->getImageRefs(cel->imageIndex());

  // If the image is only used by this cel, we can remove the image
  // from the stock.
  if (refs == 1)
    removeImageFromStock(sprite, cel->imageIndex());

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::RemoveCel(getObjects(),
        layer, cel));

  // Remove the cel from the layer.
  layer->removeCel(cel);

  // and here we destroy the cel
  delete cel;
}

void DocumentApi::setCelFramePosition(LayerImage* layer, Cel* cel, FrameNumber frame)
{
  ASSERT(cel);
  ASSERT(frame >= 0);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetCelFrame(getObjects(), layer, cel));

  layer->moveCel(cel, frame);

  doc::DocumentEvent ev(m_document);
  ev.sprite(layer->sprite());
  ev.layer(layer);
  ev.cel(cel);
  ev.frame(frame);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelFrameChanged, ev);
}

void DocumentApi::setCelPosition(Sprite* sprite, Cel* cel, int x, int y)
{
  ASSERT(cel);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetCelPosition(getObjects(), cel));

  cel->setPosition(x, y);

  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.cel(cel);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelPositionChanged, ev);
}

void DocumentApi::setCelOpacity(Sprite* sprite, Cel* cel, int newOpacity)
{
  ASSERT(cel);
  ASSERT(sprite->supportAlpha());

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetCelOpacity(getObjects(), cel));

  cel->setOpacity(newOpacity);

  doc::DocumentEvent ev(m_document);
  ev.sprite(sprite);
  ev.cel(cel);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelOpacityChanged, ev);
}

void DocumentApi::cropCel(Sprite* sprite, Cel* cel, int x, int y, int w, int h, color_t bgcolor)
{
  Image* cel_image = cel->image();
  ASSERT(cel_image);

  // create the new image through a crop
  Image* new_image = crop_image(cel_image, x-cel->x(), y-cel->y(), w, h, bgcolor);

  // replace the image in the stock that is pointed by the cel
  replaceStockImage(sprite, cel->imageIndex(), new_image);

  // update the cel's position
  setCelPosition(sprite, cel, x, y);
}

void DocumentApi::moveCel(
  LayerImage* srcLayer, FrameNumber srcFrame,
  LayerImage* dstLayer, FrameNumber dstFrame,
  color_t bgcolor)
{
  ASSERT(srcLayer != NULL);
  ASSERT(dstLayer != NULL);

  Sprite* srcSprite = srcLayer->sprite();
  Sprite* dstSprite = dstLayer->sprite();
  ASSERT(srcSprite != NULL);
  ASSERT(dstSprite != NULL);

  ASSERT(srcFrame >= 0 && srcFrame < srcSprite->totalFrames());
  ASSERT(dstFrame >= 0 && dstFrame < dstSprite->totalFrames());

  // Background to any other layer, we use copyCel() instead.
  if (srcLayer->isBackground()) {
    copyCel(srcLayer, srcFrame, dstLayer, dstFrame, bgcolor);
    return;
  }
  // In this we copy from a transparent layer to another layer...

  // Remove the dstCel (if it exists) because it must be replaced with
  // srcCel.
  Cel* srcCel = srcLayer->getCel(srcFrame);
  Cel* dstCel = dstLayer->getCel(dstFrame);
  if ((dstCel != NULL) && (!dstLayer->isBackground() || srcCel != NULL))
    removeCel(dstLayer, dstCel);

  if (srcCel != NULL) {
    // Move the cel in the same layer.
    if (srcLayer == dstLayer) {
      setCelFramePosition(srcLayer, srcCel, dstFrame);
    }
    // Move the cel between different layers.
    else {
      Cel* newCel = new Cel(*srcCel);
      newCel->setFrame(dstFrame);

      // If we are moving a cel from a transparent layer to the
      // background layer, we have to clear the background of the
      // image.
      ASSERT(!srcLayer->isBackground());
      if (dstLayer->isBackground()) {
        Image* srcImage = srcCel->image();
        Image* dstImage = crop_image(srcImage,
          -srcCel->x(),
          -srcCel->y(),
          dstSprite->width(),   // TODO dstSprite or srcSprite
          dstSprite->height(), 0);

        clear_image(dstImage, bgcolor);
        composite_image(dstImage, srcImage, srcCel->x(), srcCel->y(), 255, BLEND_MODE_NORMAL);

        newCel->setPosition(0, 0);
        newCel->setOpacity(255);
        newCel->setImage(addImageInStock(dstSprite, dstImage));
      }

      // Add and the remove, so the Stock's image is reused.
      addCel(dstLayer, newCel);
      removeCel(srcLayer, srcCel);
    }
  }

  m_document->notifyCelMoved(srcLayer, srcFrame, dstLayer, dstFrame);
}

void DocumentApi::copyCel(
  LayerImage* srcLayer, FrameNumber srcFrame,
  LayerImage* dstLayer, FrameNumber dstFrame, color_t bgcolor)
{
  ASSERT(srcLayer != NULL);
  ASSERT(dstLayer != NULL);

  Sprite* srcSprite = srcLayer->sprite();
  Sprite* dstSprite = dstLayer->sprite();
  ASSERT(srcSprite != NULL);
  ASSERT(dstSprite != NULL);

  ASSERT(srcFrame >= 0 && srcFrame < srcSprite->totalFrames());
  ASSERT(dstFrame >= 0 && dstFrame < dstSprite->totalFrames());

  Cel* srcCel = srcLayer->getCel(srcFrame);
  Cel* dstCel = dstLayer->getCel(dstFrame);

  // Remove the 'dstCel' (if it exists) because it must be replaced
  // with 'srcCel'
  if ((dstCel != NULL) && (!dstLayer->isBackground() || srcCel != NULL))
    removeCel(dstLayer, dstCel);

  // Move the cel in the same layer.
  if (srcCel != NULL) {
    Image *srcImage = srcCel->image();
    Image *dstImage;
    int dstCel_x;
    int dstCel_y;
    int dstCel_opacity;

    // If we are moving a cel from a transparent layer to the
    // background layer, we have to clear the background of the image.
    if (!srcLayer->isBackground() &&
        dstLayer->isBackground()) {
      dstImage = crop_image(srcImage,
        -srcCel->x(),
        -srcCel->y(),
        dstSprite->width(),     // TODO is dstSprite or srcSprite?
        dstSprite->height(), 0);

      clear_image(dstImage, bgcolor);
      composite_image(dstImage, srcImage, srcCel->x(), srcCel->y(), 255, BLEND_MODE_NORMAL);

      dstCel_x = 0;
      dstCel_y = 0;
      dstCel_opacity = 255;
    }
    else {
      dstImage = Image::createCopy(srcImage);
      dstCel_x = srcCel->x();
      dstCel_y = srcCel->y();
      dstCel_opacity = srcCel->opacity();
    }

    // Add the image in the stock
    int image_index = addImageInStock(dstSprite, dstImage);
    
    // Create the new cel
    dstCel = new Cel(dstFrame, image_index);
    dstCel->setPosition(dstCel_x, dstCel_y);
    dstCel->setOpacity(dstCel_opacity);

    addCel(dstLayer, dstCel);
  }

  m_document->notifyCelCopied(srcLayer, srcFrame, dstLayer, dstFrame);
}

LayerImage* DocumentApi::newLayer(Sprite* sprite)
{
  LayerImage* layer = new LayerImage(sprite);

  addLayer(sprite->folder(), layer,
           sprite->folder()->getLastLayer());

  return layer;
}

LayerFolder* DocumentApi::newLayerFolder(Sprite* sprite)
{
  LayerFolder* layer = new LayerFolder(sprite);

  addLayer(sprite->folder(), layer,
           sprite->folder()->getLastLayer());

  return layer;
}

void DocumentApi::addLayer(LayerFolder* folder, Layer* newLayer, Layer* afterThis)
{
  // Add undoers.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::AddLayer(getObjects(),
                                                m_document, newLayer));

  // Do the action.
  folder->addLayer(newLayer);
  folder->stackLayer(newLayer, afterThis);

  // Notify observers.
  doc::DocumentEvent ev(m_document);
  ev.sprite(folder->sprite());
  ev.layer(newLayer);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onAddLayer, ev);
}

void DocumentApi::removeLayer(Layer* layer)
{
  ASSERT(layer != NULL);

  // Notify observers that a layer will be removed (e.g. an Editor can
  // select another layer if the removed layer is the active one).
  doc::DocumentEvent ev(m_document);
  ev.sprite(layer->sprite());
  ev.layer(layer);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onBeforeRemoveLayer, ev);

  // Add undoers.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::RemoveLayer(getObjects(), m_document, layer));

  // Do the action.
  layer->parent()->removeLayer(layer);

  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onAfterRemoveLayer, ev);

  delete layer;
}

void DocumentApi::configureLayerAsBackground(LayerImage* layer)
{
  // Add undoers.
  if (undoEnabled()) {
    m_undoers->pushUndoer(new undoers::SetLayerFlags(getObjects(), layer));
    m_undoers->pushUndoer(new undoers::SetLayerName(getObjects(), layer));
    m_undoers->pushUndoer(new undoers::MoveLayer(getObjects(), layer));
  }

  // Do the action.
  layer->configureAsBackground();
}

void DocumentApi::restackLayerAfter(Layer* layer, Layer* afterThis)
{
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::MoveLayer(getObjects(), layer));

  layer->parent()->stackLayer(layer, afterThis);

  doc::DocumentEvent ev(m_document);
  ev.sprite(layer->sprite());
  ev.layer(layer);
  m_document->notifyObservers<doc::DocumentEvent&>(&doc::DocumentObserver::onLayerRestacked, ev);
}

void DocumentApi::restackLayerBefore(Layer* layer, Layer* beforeThis)
{
  LayerIndex beforeThisIdx = layer->sprite()->layerToIndex(beforeThis);
  LayerIndex afterThisIdx = beforeThisIdx.previous();

  restackLayerAfter(layer, layer->sprite()->indexToLayer(afterThisIdx));
}

void DocumentApi::cropLayer(Layer* layer, int x, int y, int w, int h, color_t bgcolor)
{
  if (!layer->isImage())
    return;

  if (!layer->isBackground())
    bgcolor = 0;                // TODO use proper mask color

  Sprite* sprite = layer->sprite();
  CelIterator it = ((LayerImage*)layer)->getCelBegin();
  CelIterator end = ((LayerImage*)layer)->getCelEnd();
  for (; it != end; ++it)
    cropCel(sprite, *it, x, y, w, h, bgcolor);
}

// Moves every frame in @a layer with the offset (@a dx, @a dy).
void DocumentApi::displaceLayers(Layer* layer, int dx, int dy)
{
  switch (layer->type()) {

    case OBJECT_LAYER_IMAGE: {
      CelIterator it = ((LayerImage*)layer)->getCelBegin();
      CelIterator end = ((LayerImage*)layer)->getCelEnd();
      for (; it != end; ++it) {
        Cel* cel = *it;
        setCelPosition(layer->sprite(), cel, cel->x()+dx, cel->y()+dy);
      }
      break;
    }

    case OBJECT_LAYER_FOLDER: {
      LayerIterator it = ((LayerFolder*)layer)->getLayerBegin();
      LayerIterator end = ((LayerFolder*)layer)->getLayerEnd();
      for (; it != end; ++it)
        displaceLayers(*it, dx, dy);
      break;
    }

  }
}

void DocumentApi::backgroundFromLayer(LayerImage* layer, color_t bgcolor)
{
  ASSERT(layer);
  ASSERT(layer->isImage());
  ASSERT(layer->isReadable());
  ASSERT(layer->isWritable());
  ASSERT(layer->sprite() != NULL);
  ASSERT(layer->sprite()->backgroundLayer() == NULL);

  Sprite* sprite = layer->sprite();

  // create a temporary image to draw each frame of the new
  // `Background' layer
  base::UniquePtr<Image> bg_image_wrap(Image::create(sprite->pixelFormat(),
                                               sprite->width(),
                                               sprite->height()));
  Image* bg_image = bg_image_wrap.get();

  CelIterator it = layer->getCelBegin();
  CelIterator end = layer->getCelEnd();

  for (; it != end; ++it) {
    Cel* cel = *it;

    // get the image from the sprite's stock of images
    Image* cel_image = cel->image();
    ASSERT(cel_image);

    clear_image(bg_image, bgcolor);
    composite_image(bg_image, cel_image,
                    cel->x(),
                    cel->y(),
                    MID(0, cel->opacity(), 255),
                    layer->getBlendMode());

    // now we have to copy the new image (bg_image) to the cel...
    setCelPosition(sprite, cel, 0, 0);

    // same size of cel-image and bg-image
    if (bg_image->width() == cel_image->width() &&
        bg_image->height() == cel_image->height()) {
      if (undoEnabled())
        m_undoers->pushUndoer(new undoers::ImageArea(getObjects(),
          cel_image, 0, 0, cel_image->width(), cel_image->height()));

      copy_image(cel_image, bg_image, 0, 0);
    }
    else {
      replaceStockImage(sprite, cel->imageIndex(), Image::createCopy(bg_image));
    }
  }

  // Fill all empty cels with a flat-image filled with bgcolor
  for (FrameNumber frame(0); frame<sprite->totalFrames(); ++frame) {
    Cel* cel = layer->getCel(frame);
    if (!cel) {
      Image* cel_image = Image::create(sprite->pixelFormat(), sprite->width(), sprite->height());
      clear_image(cel_image, bgcolor);

      // Add the new image in the stock
      int image_index = addImageInStock(sprite, cel_image);

      // Create the new cel and add it to the new background layer
      cel = new Cel(frame, image_index);
      addCel(layer, cel);
    }
  }

  configureLayerAsBackground(layer);
}

void DocumentApi::layerFromBackground(Layer* layer)
{
  ASSERT(layer != NULL);
  ASSERT(layer->isImage());
  ASSERT(layer->isReadable());
  ASSERT(layer->isWritable());
  ASSERT(layer->isBackground());
  ASSERT(layer->sprite() != NULL);
  ASSERT(layer->sprite()->backgroundLayer() != NULL);

  if (undoEnabled()) {
    m_undoers->pushUndoer(new undoers::SetLayerFlags(getObjects(), layer));
    m_undoers->pushUndoer(new undoers::SetLayerName(getObjects(), layer));
  }

  layer->setBackground(false);
  layer->setMoveable(true);
  layer->setName("Layer 0");
}

void DocumentApi::flattenLayers(Sprite* sprite, color_t bgcolor)
{
  Image* cel_image;
  Cel* cel;

  // Create a temporary image.
  base::UniquePtr<Image> image_wrap(Image::create(sprite->pixelFormat(),
                                            sprite->width(),
                                            sprite->height()));
  Image* image = image_wrap.get();

  // Get the background layer from the sprite.
  LayerImage* background = sprite->backgroundLayer();
  if (!background) {
    // If there aren't a background layer we must to create the background.
    background = new LayerImage(sprite);

    addLayer(sprite->folder(), background, NULL);
    configureLayerAsBackground(background);
  }

  // Copy all frames to the background.
  for (FrameNumber frame(0); frame<sprite->totalFrames(); ++frame) {
    // Clear the image and render this frame.
    clear_image(image, bgcolor);
    layer_render(sprite->folder(), image, 0, 0, frame);

    cel = background->getCel(frame);
    if (cel) {
      cel_image = cel->image();
      ASSERT(cel_image != NULL);

      // We have to save the current state of `cel_image' in the undo.
      if (undoEnabled()) {
        Dirty* dirty = new Dirty(cel_image, image, image->bounds());
        dirty->saveImagePixels(cel_image);
        m_undoers->pushUndoer(new undoers::DirtyArea(
            getObjects(), cel_image, dirty));
        delete dirty;
      }
    }
    else {
      // If there aren't a cel in this frame in the background, we
      // have to create a copy of the image for the new cel.
      cel_image = Image::createCopy(image);
      // TODO error handling: if createCopy throws

      // Here we create the new cel (with the new image `cel_image').
      cel = new Cel(frame, sprite->stock()->addImage(cel_image));
      // TODO error handling: if new Cel throws

      // And finally we add the cel in the background.
      background->addCel(cel);
    }

    copy_image(cel_image, image, 0, 0);
  }

  // Delete old layers.
  LayerList layers = sprite->folder()->getLayersList();
  LayerIterator it = layers.begin();
  LayerIterator end = layers.end();
  for (; it != end; ++it)
    if (*it != background)
      removeLayer(*it);
}

void DocumentApi::duplicateLayerAfter(Layer* sourceLayer, Layer* afterLayer)
{
  base::UniquePtr<LayerImage> newLayerPtr(new LayerImage(sourceLayer->sprite()));

  m_document->copyLayerContent(sourceLayer, m_document, newLayerPtr);

  newLayerPtr->setName(newLayerPtr->name() + " Copy");

  addLayer(sourceLayer->parent(), newLayerPtr, afterLayer);

  // Release the pointer as it is owned by the sprite now.
  newLayerPtr.release();
}

void DocumentApi::duplicateLayerBefore(Layer* sourceLayer, Layer* beforeLayer)
{
  LayerIndex beforeThisIdx = sourceLayer->sprite()->layerToIndex(beforeLayer);
  LayerIndex afterThisIdx = beforeThisIdx.previous();

  duplicateLayerAfter(sourceLayer, sourceLayer->sprite()->indexToLayer(afterThisIdx));
}

// Adds a new image in the stock. Returns the image index in the
// stock.
int DocumentApi::addImageInStock(Sprite* sprite, Image* image)
{
  ASSERT(image != NULL);

  // Do the action.
  int imageIndex = sprite->stock()->addImage(image);

  // Add undoers.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::AddImage(getObjects(),
        sprite->stock(), imageIndex));

  return imageIndex;
}

Cel* DocumentApi::addImage(LayerImage* layer, FrameNumber frameNumber, Image* image)
{
  int imageIndex = addImageInStock(layer->sprite(), image);
  base::UniquePtr<Cel> cel(new Cel(frameNumber, imageIndex));

  addCel(layer, cel);
  cel.release();

  return cel;
}

// Removes and destroys the specified image in the stock.
void DocumentApi::removeImageFromStock(Sprite* sprite, int imageIndex)
{
  ASSERT(imageIndex >= 0);

  Image* image = sprite->stock()->getImage(imageIndex);
  ASSERT(image);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::RemoveImage(getObjects(),
        sprite->stock(), imageIndex));

  sprite->stock()->removeImage(image);
  delete image;
}

void DocumentApi::replaceStockImage(Sprite* sprite, int imageIndex, Image* newImage)
{
  // Get the current image in the 'image_index' position.
  Image* oldImage = sprite->stock()->getImage(imageIndex);
  ASSERT(oldImage);

  // Replace the image in the stock.
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::ReplaceImage(getObjects(),
        sprite->stock(), imageIndex));

  sprite->stock()->replaceImage(imageIndex, newImage);
  delete oldImage;
}

// Clears the mask region in the current sprite with the specified background color.
void DocumentApi::clearMask(Layer* layer, Cel* cel, color_t bgcolor)
{
  ASSERT(cel);
  ASSERT(layer == cel->layer());

  Image* image = (cel ? cel->image(): NULL);
  if (!image)
    return;

  Mask* mask = m_document->mask();

  // If the mask is empty or is not visible then we have to clear the
  // entire image in the cel.
  if (!m_document->isMaskVisible()) {
    // If the layer is the background then we clear the image.
    if (layer->isBackground()) {
      if (undoEnabled())
        m_undoers->pushUndoer(new undoers::ImageArea(getObjects(),
          image, 0, 0, image->width(), image->height()));

      // clear all
      clear_image(image, bgcolor);
    }
    // If the layer is transparent we can remove the cel (and its
    // associated image).
    else {
      ASSERT(layer->isImage());
      removeCel(static_cast<LayerImage*>(layer), cel);
    }
  }
  else {
    int offset_x = mask->bounds().x-cel->x();
    int offset_y = mask->bounds().y-cel->y();
    int u, v, putx, puty;
    int x1 = MAX(0, offset_x);
    int y1 = MAX(0, offset_y);
    int x2 = MIN(image->width()-1, offset_x+mask->bounds().w-1);
    int y2 = MIN(image->height()-1, offset_y+mask->bounds().h-1);

    // Do nothing
    if (x1 > x2 || y1 > y2)
      return;

    if (undoEnabled())
      m_undoers->pushUndoer(new undoers::ImageArea(getObjects(),
          image, x1, y1, x2-x1+1, y2-y1+1));

    const LockImageBits<BitmapTraits> maskBits(mask->bitmap());
    LockImageBits<BitmapTraits>::const_iterator it = maskBits.begin();

    // Clear the masked zones
    for (v=0; v<mask->bounds().h; ++v) {
      for (u=0; u<mask->bounds().w; ++u, ++it) {
        ASSERT(it != maskBits.end());
        if (*it) {
          putx = u + offset_x;
          puty = v + offset_y;
          put_pixel(image, putx, puty, bgcolor);
        }
      }
    }

    ASSERT(it == maskBits.end());
  }
}

void DocumentApi::flipImage(Image* image, const gfx::Rect& bounds,
  raster::algorithm::FlipType flipType)
{
  // Insert the undo operation.
  if (undoEnabled()) {
    m_undoers->pushUndoer
      (new undoers::FlipImage
       (getObjects(), image, bounds, flipType));
  }

  // Flip the portion of the bitmap.
  raster::algorithm::flip_image(image, bounds, flipType);
}

void DocumentApi::flipImageWithMask(Image* image, const Mask* mask, raster::algorithm::FlipType flipType, color_t bgcolor)
{
  base::UniquePtr<Image> flippedImage((Image::createCopy(image)));

  // Flip the portion of the bitmap.
  raster::algorithm::flip_image_with_mask(flippedImage, mask, flipType, bgcolor);

  // Insert the undo operation.
  if (undoEnabled()) {
    base::UniquePtr<Dirty> dirty((new Dirty(image, flippedImage, image->bounds())));
    dirty->saveImagePixels(image);

    m_undoers->pushUndoer(new undoers::DirtyArea(getObjects(), image, dirty));
  }

  // Copy the flipped image into the image specified as argument.
  copy_image(image, flippedImage, 0, 0);
}

void DocumentApi::pasteImage(Sprite* sprite, Cel* cel, const Image* src_image, int x, int y, int opacity)
{
  ASSERT(cel != NULL);

  Image* cel_image = cel->image();
  Image* cel_image2 = Image::createCopy(cel_image);
  composite_image(cel_image2, src_image, x-cel->x(), y-cel->y(), opacity, BLEND_MODE_NORMAL);

  replaceStockImage(sprite, cel->imageIndex(), cel_image2); // TODO fix this, improve, avoid replacing the whole image
}

void DocumentApi::copyToCurrentMask(Mask* mask)
{
  ASSERT(m_document->mask());
  ASSERT(mask);

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetMask(getObjects(),
        m_document));

  m_document->mask()->copyFrom(mask);
}

void DocumentApi::setMaskPosition(int x, int y)
{
  ASSERT(m_document->mask());

  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetMaskPosition(getObjects(), m_document));

  m_document->mask()->setOrigin(x, y);
  m_document->resetTransformation();
}

void DocumentApi::deselectMask()
{
  if (undoEnabled())
    m_undoers->pushUndoer(new undoers::SetMask(getObjects(),
        m_document));

  m_document->setMaskVisible(false);
}

void DocumentApi::setPalette(Sprite* sprite, FrameNumber frame, Palette* newPalette)
{
  Palette* currentSpritePalette = sprite->getPalette(frame); // Sprite current pal
  int from, to;

  // Check differences between current sprite palette and current system palette
  from = to = -1;
  currentSpritePalette->countDiff(newPalette, &from, &to);

  if (from >= 0 && to >= from) {
    // Add undo information to save the range of pal entries that will be modified.
    if (undoEnabled()) {
      m_undoers->pushUndoer
        (new undoers::SetPaletteColors(getObjects(),
                                       sprite, currentSpritePalette,
                                       frame, from, to));
    }

    // Change the sprite palette
    sprite->setPalette(newPalette, false);
  }
}

bool DocumentApi::undoEnabled()
{
  return
    m_undoers != NULL &&
    m_document->getUndo()->isEnabled();
}

} // namespace app
