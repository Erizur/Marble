/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::{PremultipliedColorF, RasterSpace, Shadow};
use crate::scene_building::{CreateShadow, IsVisible};
use crate::intern;
use crate::internal_types::LayoutPrimitiveInfo;
use crate::prim_store::{
    PrimKey, InternablePrimitive, PrimitiveStore, PrimitiveKind,
    PrimTemplate, PrimTemplateCommonData, PrimitiveOpacity,
};
use crate::frame_builder::FrameBuildingState;
use crate::scene::SceneProperties;
use std::ops;

#[derive(Debug, Clone, Eq, MallocSizeOf, PartialEq, Hash)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct ClearPrim;

pub type ClearKey = PrimKey<ClearPrim>;

pub type ClearDataHandle = intern::Handle<ClearPrim>;

impl ClearKey {
    pub fn new(info: &LayoutPrimitiveInfo) -> Self {
        ClearKey { common: info.into(), kind: ClearPrim }
    }
}

impl intern::InternDebug for ClearKey {}

impl intern::Internable for ClearPrim {
    type Key = ClearKey;
    type StoreData = ClearTemplate;
    type InternData = ();
    const PROFILE_COUNTER: usize = crate::profiler::INTERNED_PRIMITIVES;
}

impl InternablePrimitive for ClearPrim {
    fn into_key(
        self,
        info: &LayoutPrimitiveInfo,
    ) -> ClearKey {
        ClearKey::new(info)
    }

    fn make_instance_kind(
        _key: ClearKey,
        data_handle: ClearDataHandle,
        _prim_store: &mut PrimitiveStore,
    ) -> PrimitiveKind {
        PrimitiveKind::Clear {
            data_handle,
        }
    }
}

impl IsVisible for ClearPrim {
    fn is_visible(&self) -> bool {
        true
    }
}

impl CreateShadow for ClearPrim {
    fn create_shadow(
        &self,
        _: &Shadow,
        _: bool,
        _: RasterSpace,
    ) -> ClearPrim {
        ClearPrim
    }
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
pub struct ClearData;

pub type ClearTemplate = PrimTemplate<ClearData>;

impl ops::Deref for ClearTemplate {
    type Target = PrimTemplateCommonData;
    fn deref(&self) -> &Self::Target {
        &self.common
    }
}

impl ops::DerefMut for ClearTemplate {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.common
    }
}

impl From<ClearKey> for ClearTemplate {
    fn from(item: ClearKey) -> Self {
        ClearTemplate {
            common: PrimTemplateCommonData::with_key_common(item.common),
            kind: ClearData,
        }
    }
}

impl ClearTemplate {
    pub fn update(
        &mut self,
        frame_state: &mut FrameBuildingState,
        _scene_properties: &SceneProperties,
    ) {
        let mut writer = frame_state.frame_gpu_data.f32.write_blocks(1);
        writer.push_one(PremultipliedColorF::BLACK);
        self.common.gpu_buffer_address = writer.finish();
        self.opacity = PrimitiveOpacity::translucent();
    }
}
